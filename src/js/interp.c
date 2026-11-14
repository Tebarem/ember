/*
 * Interpreter: the dispatch loop, call frames on a segmented value stack, closures and native
 * functions, exception unwinding with error locations, and the embedder entry points (realms,
 * eval, native functions, limits).
 *
 * JS-to-JS calls do not recurse in C: they push a frame and continue in the same loop. A native
 * call or an embedder call re-enters run(), and that re-entry is what the native stack limit
 * bounds.
 */
#include "js/builtins.h"
#include "js/collections.h"
#include "js/compiler.h"
#include "js/ic.h"
#include "js/interp.h"
#include "js/module.h"
#include "js/ops.h"
#include "js/string.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(__GNUC__) && !defined(EMBI_JS_NO_COMPUTED_GOTO)
#define EMBI_JS_COMPUTED_GOTO 1
#else
#define EMBI_JS_COMPUTED_GOTO 0
#endif

#define UNDEF EMBI_JS_UNDEFINED
#define EXC EMBI_JS_EXCEPTION
#define ATTR_W EMBI_JS_ATTR_WRITABLE
#define ATTR_E EMBI_JS_ATTR_ENUMERABLE
#define ATTR_C EMBI_JS_ATTR_CONFIGURABLE

static size_t arr_bytes(size_t n, size_t elem)
{
    return (n ? n : 1u) * elem;
}

/* Bytes of a closure record with its upvalue array in the same block (J5m2). J5n2: the home/init
 * fields are a separate part (embi_js_closure_ext) that only some closures have, and an empty upvalue
 * array takes no space. */
static size_t closure_bytes(uint32_t nupv, bool ext)
{
    return sizeof(embi_js_closure) + (ext ? sizeof(embi_js_closure_ext) : 0u) +
           (size_t)nupv * sizeof(embi_js_value);
}

static embi_js_value intrinsic(embi_js_runtime *rt, enum embi_js_proto which)
{
    embi_js_value v = rt->protos[which];
    return embi_js_is_empty(v) ? EMBI_JS_NULL : v;
}

static embi_js_code *code_of(embi_js_value v)
{
    return (embi_js_code *)embi_js_as_cell(v);
}

static embi_js_closure *closure_of(const embi_js_object *o)
{
    return (embi_js_closure *)o->internal;
}

static embi_js_closure_ext *cl_ext(const embi_js_closure *cl)
{
    return cl->has_ext ? (embi_js_closure_ext *)(cl + 1) : nullptr;
}

static embi_js_value cl_home(const embi_js_closure *cl)
{
    const embi_js_closure_ext *e = cl_ext(cl);
    return e ? e->home : EMBI_JS_UNDEFINED;
}

static embi_js_value cl_pinit(const embi_js_closure *cl)
{
    const embi_js_closure_ext *e = cl_ext(cl);
    return e ? e->pinit : EMBI_JS_UNDEFINED;
}

static embi_js_value cl_finit(const embi_js_closure *cl)
{
    const embi_js_closure_ext *e = cl_ext(cl);
    return e ? e->finit : EMBI_JS_UNDEFINED;
}

/* The ext of the closure o, added when it has none: the block is reallocated with the ext in front of
 * the upvalues (o->internal changes, so a caller must not hold the old record across this call). The
 * closures that are given a home or initializers are the ones just made or being defined, never the
 * running callee, whose upvalue pointer the interpreter caches. */
static embi_js_closure_ext *cl_ext_ensure(embi_js_object *o)
{
    embi_js_closure *cl = closure_of(o);
    if (cl->has_ext) {
        return cl_ext(cl);
    }
    size_t old_bytes = closure_bytes(cl->nupvals, false);
    embi_js_closure *nc = embi_calloc(1, closure_bytes(cl->nupvals, true));
    nc->code = cl->code;
    nc->nupvals = cl->nupvals;
    nc->has_ext = 1;
    embi_js_closure_ext *e = (embi_js_closure_ext *)(nc + 1);
    e->home = e->pinit = e->finit = EMBI_JS_UNDEFINED;
    nc->upvals = (embi_js_value *)(e + 1);
    for (uint32_t i = 0; i < cl->nupvals; i++) {
        nc->upvals[i] = cl->upvals[i];
    }
    embi_free(cl, old_bytes);
    o->internal = nc;
    EMBI_JS_WB_STRUCT(o);
    return e;
}

static void cl_set_home(embi_js_object *o, embi_js_value v)
{
    embi_js_closure_ext *e = cl_ext_ensure(o);
    EMBI_JS_STORE(o, e->home, v);
}

static embi_js_value *closure_upvals(const embi_js_object *o)
{
    return o && o->internal ? closure_of(o)->upvals : nullptr;
}

/* ------------------------------------------------------------------ VM lifetime and roots */

static void vm_mark(embi_js_tracer *t, void *user)
{
    embi_js_runtime *rt = user;
    embi_js_vm *vm = rt->vm;
    if (!vm) {
        return;
    }
    embi_js_mark_value(t, vm->loc_value);
    embi_js_mark_value(t, vm->sav_value);
    if (vm->realm && vm->realm->global) {
        embi_js_mark_cell(t, &vm->realm->global->hdr);
    }
    for (uint32_t i = 0; i <= vm->cur && i < vm->chunks.len; i++) {
        const embi_js_stack_chunk *ch = &vm->chunks.data[i];
        for (embi_js_value *v = ch->base; v < ch->top; v++) {
            embi_js_mark_value(t, *v);
        }
    }
    for (uint32_t i = 0; i < vm->frames.len; i++) {
        const embi_js_frame *f = &vm->frames.data[i];
        embi_js_mark_cell(t, &f->code->hdr);
        if (f->callee) {
            embi_js_mark_cell(t, &f->callee->hdr);
        }
        embi_js_mark_value(t, f->this_v);
        embi_js_mark_value(t, f->new_target);
        if (f->gen) {
            embi_js_mark_cell(t, &f->gen->hdr);
        }
    }
}

static embi_js_stack_chunk new_chunk(size_t values)
{
    embi_js_stack_chunk ch;
    ch.base = embi_alloc(values * sizeof(embi_js_value));
    ch.top = ch.base;
    ch.end = ch.base + values;
    ch.cap = values;
    return ch;
}

embi_js_vm *embi_js_vm_ensure(embi_js_runtime *rt)
{
    if (rt->vm) {
        return rt->vm;
    }
    embi_js_vm *vm = embi_calloc(1, sizeof *vm);
    vm->max_depth = EMBI_JS_DEFAULT_MAX_DEPTH;
    vm->native_limit = EMBI_JS_DEFAULT_NATIVE_BYTES;
    embi_vec_push(&vm->chunks, new_chunk(EMBI_JS_STACK_FIRST_VALUES));
    rt->vm = vm;
    embi_js_set_type_ops(rt, EMBI_JS_CELL_CODE,
                         (embi_js_type_ops){embi_js_code_trace, embi_js_code_finalize});
    embi_js_root_add(rt, vm_mark, rt);
    return vm;
}

void embi_js_vm_free(embi_js_runtime *rt)
{
    embi_js_vm *vm = rt->vm;
    if (!vm) {
        return;
    }
    for (uint32_t i = 0; i < vm->chunks.len; i++) {
        embi_free(vm->chunks.data[i].base, vm->chunks.data[i].cap * sizeof(embi_js_value));
    }
    embi_vec_free(&vm->chunks);
    embi_vec_free(&vm->frames);
    if (vm->loc_file) {
        embi_free(vm->loc_file, strlen(vm->loc_file) + 1u);
    }
    if (vm->sav_file) {
        embi_free(vm->sav_file, strlen(vm->sav_file) + 1u);
    }
    embi_free(vm, sizeof *vm);
    rt->vm = nullptr;
}

/* Value stack: frames take contiguous runs and never span chunks. */
static embi_js_value *alloc_slots(embi_js_vm *vm, uint32_t n, uint32_t *chunk_out,
                                  uint32_t *prev_out)
{
    *prev_out = vm->cur;
    if ((size_t)(vm->chunks.data[vm->cur].end - vm->chunks.data[vm->cur].top) < n) {
        uint32_t next = vm->cur + 1u;
        if (next == vm->chunks.len) {
            size_t values = n > EMBI_JS_STACK_NEXT_VALUES ? n : EMBI_JS_STACK_NEXT_VALUES;
            embi_vec_push(&vm->chunks, new_chunk(values));
        } else if (vm->chunks.data[next].cap < n) {
            embi_js_stack_chunk *old = &vm->chunks.data[next];
            embi_free(old->base, old->cap * sizeof(embi_js_value));
            *old = new_chunk(n);
        }
        vm->chunks.data[next].top = vm->chunks.data[next].base;
        vm->cur = next;
    }
    embi_js_stack_chunk *ch = &vm->chunks.data[vm->cur];
    embi_js_value *base = ch->top;
    ch->top += n;
    *chunk_out = vm->cur;
    return base;
}

static void pop_frame(embi_js_vm *vm)
{
    embi_js_frame *f = &vm->frames.data[vm->frames.len - 1u];
    vm->chunks.data[f->chunk].top = f->base;
    vm->cur = f->prev_chunk;
    vm->frames.len--;
}

/* ------------------------------------------------------------------ errors and locations */

static void sb_add(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

static void set_location(embi_js_vm *vm, embi_js_value exc, const char *file, uint32_t line,
                         uint32_t col)
{
    vm->loc_valid = true;
    vm->loc_value = exc;
    vm->loc_line = line;
    vm->loc_col = col;
    const char *name = file ? file : "";
    size_t n = strlen(name);
    /* Most throws come from the same file: keep its buffer when the name has not changed. */
    if (vm->loc_file && strcmp(vm->loc_file, name) == 0) {
        return;
    }
    if (vm->loc_file) {
        embi_free(vm->loc_file, strlen(vm->loc_file) + 1u);
    }
    vm->loc_file = embi_alloc(n + 1u);
    memcpy(vm->loc_file, name, n + 1u);
}

/* A finally entry keeps the exception's location aside: cleanup code in the block may run its
 * own try/catch, which clears the live record. */
static void save_location(embi_js_vm *vm)
{
    if (!vm->loc_valid) {
        vm->sav_valid = false;
        return;
    }
    const char *name = vm->loc_file ? vm->loc_file : "";
    size_t n = strlen(name);
    if (vm->sav_file) {
        embi_free(vm->sav_file, strlen(vm->sav_file) + 1u);
    }
    vm->sav_file = embi_alloc(n + 1u);
    memcpy(vm->sav_file, name, n + 1u);
    vm->sav_valid = true;
    vm->sav_value = vm->loc_value;
    vm->sav_line = vm->loc_line;
    vm->sav_col = vm->loc_col;
}

static void restore_location(embi_js_vm *vm, embi_js_value exc)
{
    if (vm->loc_valid && vm->loc_value == exc) {
        return;
    }
    if (vm->sav_valid && vm->sav_value == exc) {
        set_location(vm, exc, vm->sav_file, vm->sav_line, vm->sav_col);
    }
}

/* Gives an error object without a stack property a `stack` string: a header line and one line
 * per frame, innermost first. codes and offs give each frame's CODE and current offset. */
static void attach_stack(embi_js_runtime *rt, embi_js_value exc, embi_js_code *const *codes,
                         const uint32_t *offs, uint32_t n)
{
    if (!embi_js_is_object(exc)) {
        return;
    }
    embi_js_object *e = embi_js_as_object(exc);
    if (e->klass != &embi_js_class_error) {
        return;
    }
    embi_js_key sk = embi_js_key_c(rt, "stack");
    embi_js_pdesc own;
    if (embi_js_obj_get_own_property(rt, e, sk, &own) != EMBI_JS_NO) {
        return;
    }
    embi_js_value saved = rt->pending_exception;
    embi_js_vm *vm = rt->vm;
    /* A user accessor on name/message may run JS and throw: keep the live location record. */
    bool lv = vm && vm->loc_valid;
    embi_js_value lval = vm ? vm->loc_value : 0;
    uint32_t lline = vm ? vm->loc_line : 0;
    uint32_t lcol = vm ? vm->loc_col : 0;
    char *lfile = nullptr;
    if (vm && vm->loc_file) {
        size_t fl = strlen(vm->loc_file);
        lfile = embi_alloc(fl + 1u);
        memcpy(lfile, vm->loc_file, fl + 1u);
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_value nm = embi_js_get(rt, e, embi_js_name_val(rt, EMBI_JS_NAME_NAME));
    embi_js_value msg = embi_js_get(rt, e, embi_js_name_val(rt, EMBI_JS_NAME_MESSAGE));
    if (embi_js_is_string(nm)) {
        embi_js_str_append_utf8(embi_js_as_string(nm), &sb);
    } else {
        sb_add(&sb, "Error");
    }
    if (embi_js_is_string(msg) && embi_js_str_len(embi_js_as_string(msg)) > 0) {
        sb_add(&sb, ": ");
        embi_js_str_append_utf8(embi_js_as_string(msg), &sb);
    }
    for (uint32_t i = 0; i < n && i < 10u; i++) {
        const embi_js_code *c = codes[i];
        uint32_t line = 0;
        uint32_t col = 0;
        (void)embi_js_code_position(c, offs[i], &line, &col);
        sb_add(&sb, "\n    at ");
        if (embi_js_is_string(c->name)) {
            embi_js_str_append_utf8(embi_js_as_string(c->name), &sb);
        } else {
            sb_add(&sb, "<anonymous>");
        }
        sb_add(&sb, " (");
        embi_sb_append(&sb, (embi_str){c->file ? c->file : "", c->file ? (uint32_t)strlen(c->file) : 0u});
        char num[48];
        snprintf(num, sizeof num, ":%u:%u)", line, col);
        sb_add(&sb, num);
    }
    embi_js_string *str = embi_js_str_new_utf8(rt, embi_sb_cstr(&sb), sb.len);
    embi_js_define_raw(rt, e, sk, embi_js_from_string(str), ATTR_W | ATTR_C);
    embi_sb_free(&sb);
    rt->pending_exception = saved;
    if (vm && lv) {
        set_location(vm, lval, lfile, lline, lcol);
    }
    if (lfile) {
        embi_free(lfile, strlen(lfile) + 1u);
    }
}

/* Records where exc was thrown: the top frame at top_off. The record stays while the same
 * exception keeps propagating. */
static void record_throw(embi_js_runtime *rt, embi_js_vm *vm, embi_js_value exc, uint32_t top_off)
{
    if (vm->loc_valid && vm->loc_value == exc) {
        return;
    }
    uint32_t n = vm->frames.len;
    if (n == 0) {
        return;
    }
    embi_js_code *codes[10];
    uint32_t offs[10];
    uint32_t m = 0;
    for (uint32_t i = 0; i < n && m < 10u; i++) {
        const embi_js_frame *f = &vm->frames.data[n - 1u - i];
        codes[m] = f->code;
        if (i == 0) {
            offs[m] = top_off;
        } else if (f->cur) {
            offs[m] = (uint32_t)(f->cur - f->code->bc);
        } else {
            uint32_t o = (uint32_t)(f->pc - f->code->bc);
            offs[m] = o > 0 ? o - 1u : 0;
        }
        m++;
    }
    embi_js_code *top = vm->frames.data[n - 1u].code;
    uint32_t line = 0;
    uint32_t col = 0;
    (void)embi_js_code_position(top, top_off, &line, &col);
    set_location(vm, exc, top->file, line, col);
    attach_stack(rt, exc, codes, offs, m);
}

void embi_js_note_location(embi_js_runtime *rt, const char *file, uint32_t line, uint32_t col)
{
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    set_location(vm, rt->pending_exception, file, line, col);
    attach_stack(rt, rt->pending_exception, nullptr, nullptr, 0);
}

bool embi_js_error_location(embi_js_runtime *rt, const char **file, uint32_t *line, uint32_t *col)
{
    embi_js_vm *vm = rt->vm;
    if (!vm || !vm->loc_valid) {
        return false;
    }
    *file = vm->loc_file ? vm->loc_file : "";
    *line = vm->loc_line;
    *col = vm->loc_col;
    return true;
}

static embi_js_value throw_name(embi_js_runtime *rt, enum embi_js_proto which, const char *pre,
                                embi_js_value name, const char *post)
{
    embi_sb sb = {nullptr, 0, 0};
    sb_add(&sb, pre);
    if (embi_js_is_string(name)) {
        embi_js_str_append_utf8(embi_js_as_string(name), &sb);
    }
    sb_add(&sb, post);
    embi_js_value r = embi_js_throw_error(rt, which, "%s", embi_sb_cstr(&sb));
    embi_sb_free(&sb);
    return r;
}

/* ------------------------------------------------------------------ globals */

static embi_js_object *global_of(embi_js_runtime *rt)
{
    embi_js_vm *vm = rt->vm;
    return vm->realm->global;
}

static embi_js_value tdz_error(embi_js_runtime *rt, embi_js_value name);

/* The global declarative record (script-level let and const of the realm). Returns the box of a
 * binding, or nullptr; *is_const tells a const. */
static embi_js_box *lex_find(embi_js_runtime *rt, embi_js_key k, bool *is_const)
{
    embi_js_realm *r = rt->vm->realm;
    if (r->nlex == 0) {
        return nullptr;
    }
    embi_js_pdesc d;
    if (embi_js_obj_get_own_property(rt, r->lex, k, &d) != EMBI_JS_YES) {
        return nullptr;
    }
    *is_const = !(d.attrs & EMBI_JS_ATTR_WRITABLE);
    return (embi_js_box *)embi_js_as_cell(d.value);
}

static void var_names_add(embi_js_runtime *rt, embi_js_key k)
{
    embi_js_define_raw(rt, rt->vm->realm->vars, k, EMBI_JS_TRUE, 0);
}

static embi_js_value global_get(embi_js_runtime *rt, embi_js_key k, bool optional)
{
    embi_js_object *g = global_of(rt);
    bool lc;
    embi_js_box *lb = lex_find(rt, k, &lc);
    if (lb) {
        return embi_js_is_empty(lb->value) ? tdz_error(rt, embi_js_key_to_value(rt, k)) : lb->value;
    }
    /* Fast path: an own data property is one lookup. Everything else takes the general route. */
    embi_js_pdesc d;
    int own = embi_js_obj_get_own_property(rt, g, k, &d);
    if (own == EMBI_JS_THREW) {
        return EXC;
    }
    if (own == EMBI_JS_YES && (d.has & EMBI_JS_PD_VALUE)) {
        return d.value;
    }
    int has = embi_js_obj_has_property(rt, g, k);
    if (has == EMBI_JS_THREW) {
        return EXC;
    }
    if (!has) {
        if (optional) {
            return UNDEF;
        }
        return throw_name(rt, EMBI_JS_PROTO_REFERENCE_ERROR, "", embi_js_key_to_value(rt, k),
                          " is not defined");
    }
    return embi_js_obj_get(rt, g, k, embi_js_from_object(g));
}

static embi_js_value global_set(embi_js_runtime *rt, embi_js_key k, embi_js_value v, bool strict)
{
    embi_js_object *g = global_of(rt);
    bool lc;
    embi_js_box *lb = lex_find(rt, k, &lc);
    if (lb) {
        if (embi_js_is_empty(lb->value)) {
            return tdz_error(rt, embi_js_key_to_value(rt, k));
        }
        if (lc) {
            return embi_js_throw_type_error(rt, "Assignment to constant variable.");
        }
        lb->value = v;
        EMBI_JS_WB(lb, v);
        return UNDEF;
    }
    if (strict) {
        int has = embi_js_obj_has_property(rt, g, k);
        if (has == EMBI_JS_THREW) {
            return EXC;
        }
        if (!has) {
            return throw_name(rt, EMBI_JS_PROTO_REFERENCE_ERROR, "", embi_js_key_to_value(rt, k),
                              " is not defined");
        }
    }
    int r = embi_js_put(rt, g, k, v, strict);
    return r == EMBI_JS_THREW ? EXC : UNDEF;
}

/* Global sites (J5d). An entry records where the name was found: a binding of the lexical record
 * (G_LEX, the box), or an own property of the global object (G_OBJ, its shape and slot). Both stay
 * valid while the realm lex count is unchanged, because a later let can shadow a global property.
 * A G_OBJ entry also needs the global object shape unchanged: its dictionary shape renews on every
 * add, delete and attribute change, since the global object is flagged as a prototype. Value writes
 * keep the shape, so reads and writes of the property go straight to its slot. */
static bool global_fill(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic, bool write)
{
    embi_js_realm *r = rt->vm->realm;
    ic->gkind = EMBI_JS_IC_G_NONE;
    EMBI_ASSERT(ic->isglob || (ic->nentries == 0 && ic->more == nullptr));
    bool lc;
    embi_js_box *b = lex_find(rt, ic->key, &lc);
    if (b) {
        if (write && lc) {
            return false;
        }
        ic->isglob = 1;
        ic->gkind = EMBI_JS_IC_G_LEX;
        ic->gwritable = !lc;
        ic->gbox = b;
        ic->gepoch = r->nlex;
        ic->grealm = r;
        EMBI_JS_WB(owner, embi_js_from_cell(b));
        return true;
    }
    embi_js_object *g = r->global;
    /* An exotic global (named hooks, or a set hook that a cached write would skip) stays uncached. */
    if (!embi_js_ic_class_plain(g)) {
        return false;
    }
    if ((g->shape->hdr.flags & EMBI_JS_SHAPE_DICT) && !(g->hdr.flags & EMBI_JS_OBJ_IS_PROTO)) {
        return false;
    }
    uint32_t slot;
    uint8_t attrs;
    if (!embi_js_obj_ic_own(g, ic->key, &slot, &attrs) || (attrs & EMBI_JS_ATTR_ACCESSOR)) {
        return false;
    }
    bool writable = (attrs & EMBI_JS_ATTR_WRITABLE) != 0;
    if (write && !writable) {
        return false;
    }
    ic->isglob = 1;
    ic->gkind = EMBI_JS_IC_G_OBJ;
    ic->gwritable = writable;
    ic->gshape = g->shape;
    ic->gslot = slot;
    ic->gepoch = r->nlex;
    ic->grealm = r;
    EMBI_JS_WB(owner, embi_js_from_cell(g->shape));
    return true;
}

static embi_js_value global_get_ic(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic,
                                   bool optional)
{
    embi_js_realm *r = rt->vm->realm;
    if (ic->gkind != EMBI_JS_IC_G_NONE && ic->gepoch == r->nlex && ic->grealm == r) {
        if (ic->gkind == EMBI_JS_IC_G_LEX) {
            if (!embi_js_is_empty(ic->gbox->value)) {
                return ic->gbox->value;
            }
        } else if (ic->gshape == r->global->shape) {
            return *embi_js_obj_ic_value(r->global, ic->gslot);
        }
    }
    if (embi_js_ic_may_fill(ic) && !global_fill(rt, owner, ic, false)) {
        embi_js_ic_fill_failed(ic);
    }
    return global_get(rt, ic->key, optional);
}

static embi_js_value global_set_ic(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic,
                                   embi_js_value v, bool strict)
{
    embi_js_realm *r = rt->vm->realm;
    if (ic->gkind != EMBI_JS_IC_G_NONE && ic->gepoch == r->nlex && ic->grealm == r &&
        ic->gwritable) {
        if (ic->gkind == EMBI_JS_IC_G_LEX) {
            if (!embi_js_is_empty(ic->gbox->value)) {
                ic->gbox->value = v;
                EMBI_JS_WB(ic->gbox, v);
                return UNDEF;
            }
        } else if (ic->gshape == r->global->shape) {
            *embi_js_obj_ic_value(r->global, ic->gslot) = v;
            EMBI_JS_WB(r->global, v);
            return UNDEF;
        }
    }
    if (embi_js_ic_may_fill(ic) && !global_fill(rt, owner, ic, true)) {
        embi_js_ic_fill_failed(ic);
    }
    return global_set(rt, ic->key, v, strict);
}

/* ------------------------------------------------------------------ property access */

static embi_js_value get_prop(embi_js_runtime *rt, embi_js_value base, embi_js_key k)
{
    if (embi_js_is_object(base)) {
        return embi_js_obj_get(rt, embi_js_as_object(base), k, base);
    }
    if (embi_js_is_nullish(base)) {
        return embi_js_throw_type_error(rt, "Cannot read properties of %s",
                                        base == EMBI_JS_NULL ? "null" : "undefined");
    }
    return embi_js_get_v(rt, base, k);
}

/* GET_FIELD and GET_FIELD_THIS when the base is not an object (J5d): a string length is its own
 * length, a nullish base throws as before, and the other primitives look the method up through the
 * site cache on the intrinsic prototype, with no wrapper allocated. */
static embi_js_value get_field_prim(embi_js_runtime *rt, embi_js_cell *owner, embi_js_ic *ic,
                                    embi_js_value base)
{
    if (embi_js_is_string(base) && ic->key_length) {
        return embi_js_number((double)embi_js_str_len(embi_js_as_string(base)));
    }
    if (embi_js_is_nullish(base)) {
        return get_prop(rt, base, ic->key);
    }
    return embi_js_ic_get_prim(rt, owner, ic, base);
}

static embi_js_value put_prop(embi_js_runtime *rt, embi_js_value base, embi_js_key k, embi_js_value v,
                              bool strict)
{
    if (embi_js_is_object(base)) {
        int r = embi_js_put(rt, embi_js_as_object(base), k, v, strict);
        return r == EMBI_JS_THREW ? EXC : UNDEF;
    }
    if (embi_js_is_nullish(base)) {
        return embi_js_throw_type_error(rt, "Cannot set properties of %s",
                                        base == EMBI_JS_NULL ? "null" : "undefined");
    }
    /* PutValue on a primitive base: ToObject(base).[[Set]](k, v, base). A setter or a proxy trap on
     * the prototype chain runs with the primitive as this/receiver; a plain data property cannot be
     * created on a primitive (a TypeError in strict code). */
    embi_js_object *w = embi_js_to_object(rt, base);
    if (!w) {
        return EXC;
    }
    int r = embi_js_obj_set(rt, w, k, v, base);
    if (r == EMBI_JS_THREW) {
        return EXC;
    }
    if (r == EMBI_JS_NO && strict) {
        return embi_js_throw_type_error(rt, "Cannot create property on a primitive value");
    }
    return UNDEF;
}

static embi_js_value delete_prop(embi_js_runtime *rt, embi_js_value base, embi_js_key k, bool strict)
{
    if (embi_js_is_nullish(base)) {
        return embi_js_throw_type_error(rt, "Cannot convert undefined or null to object");
    }
    if (!embi_js_is_object(base)) {
        return EMBI_JS_TRUE;
    }
    int r = embi_js_obj_delete(rt, embi_js_as_object(base), k);
    if (r == EMBI_JS_THREW) {
        return EXC;
    }
    if (r == EMBI_JS_NO && strict) {
        return embi_js_throw_type_error(rt, "Cannot delete property");
    }
    return embi_js_from_bool(r == EMBI_JS_YES);
}

/* ------------------------------------------------------------------ operators */

/* BigInt operands of a numeric operator, both already ToNumeric'd. Mixing a BigInt with a Number
 * throws a TypeError; two BigInts go to the BigInt operator. */
static embi_js_value big_operands(embi_js_runtime *rt, enum embi_js_bigop op, embi_js_value a,
                                  embi_js_value b)
{
    if (!embi_js_is_bigint(a) || !embi_js_is_bigint(b)) {
        return embi_js_throw_type_error(
            rt, "Cannot mix BigInt and other types, use explicit conversions");
    }
    return embi_js_bigint_binop(rt, op, a, b);
}

static embi_js_value op_add(embi_js_runtime *rt, embi_js_value a, embi_js_value b)
{
    if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
        int64_t s = (int64_t)embi_js_as_int32(a) + (int64_t)embi_js_as_int32(b);
        if (s >= INT32_MIN && s <= INT32_MAX) {
            return embi_js_from_int32((int32_t)s);
        }
    }
    if (embi_js_is_number(a) && embi_js_is_number(b)) {
        return embi_js_number(embi_js_as_double(a) + embi_js_as_double(b));
    }
    embi_js_value pa = embi_js_to_primitive(rt, a, EMBI_JS_HINT_DEFAULT);
    if (pa == EXC) {
        return EXC;
    }
    embi_js_value pb = embi_js_to_primitive(rt, b, EMBI_JS_HINT_DEFAULT);
    if (pb == EXC) {
        return EXC;
    }
    if (embi_js_is_string(pa) || embi_js_is_string(pb)) {
        /* J5g: string operands stay as values, so a long result is a rope.
         * ToString runs on the left operand first, as the spec orders it. */
        embi_js_value sa = pa;
        if (!embi_js_is_string(pa)) {
            embi_js_string *t = embi_js_to_string(rt, pa);
            if (!t) {
                return EXC;
            }
            sa = embi_js_from_string(t);
        }
        embi_js_value sb = pb;
        if (!embi_js_is_string(pb)) {
            embi_js_string *t = embi_js_to_string(rt, pb);
            if (!t) {
                return EXC;
            }
            sb = embi_js_from_string(t);
        }
        return embi_js_str_concat_v(rt, sa, sb);
    }
    embi_js_value na = embi_js_to_numeric(rt, pa);
    if (na == EXC) {
        return EXC;
    }
    embi_js_value nb = embi_js_to_numeric(rt, pb);
    if (nb == EXC) {
        return EXC;
    }
    if (embi_js_is_bigint(na) || embi_js_is_bigint(nb)) {
        return big_operands(rt, EMBI_JS_BIG_ADD, na, nb);
    }
    return embi_js_number(embi_js_as_double(na) + embi_js_as_double(nb));
}

static double js_pow(double x, double y)
{
    if (y != y) {
        return NAN;
    }
    if (y == 0.0) {
        return 1.0;
    }
    if ((x == 1.0 || x == -1.0) && isinf(y)) {
        return NAN;
    }
    return pow(x, y);
}

/* Arithmetic on two values through ToNumber. op is one of the numeric opcodes. */
static enum embi_js_bigop big_op_of(uint8_t op)
{
    switch (op) {
    case EMBI_JSOP_SUB:
        return EMBI_JS_BIG_SUB;
    case EMBI_JSOP_MUL:
        return EMBI_JS_BIG_MUL;
    case EMBI_JSOP_DIV:
        return EMBI_JS_BIG_DIV;
    case EMBI_JSOP_MOD:
        return EMBI_JS_BIG_MOD;
    case EMBI_JSOP_EXP:
        return EMBI_JS_BIG_EXP;
    case EMBI_JSOP_SHL:
        return EMBI_JS_BIG_SHL;
    case EMBI_JSOP_SAR:
        return EMBI_JS_BIG_SAR;
    case EMBI_JSOP_SHR:
        return EMBI_JS_BIG_SHR;
    case EMBI_JSOP_BAND:
        return EMBI_JS_BIG_AND;
    case EMBI_JSOP_BOR:
        return EMBI_JS_BIG_OR;
    case EMBI_JSOP_BXOR:
        return EMBI_JS_BIG_XOR;
    default:
        return EMBI_JS_BIG_ADD;
    }
}

static embi_js_value op_arith(embi_js_runtime *rt, uint8_t op, embi_js_value a, embi_js_value b)
{
    embi_js_value na = embi_js_to_numeric(rt, a);
    if (na == EXC) {
        return EXC;
    }
    embi_js_value nb = embi_js_to_numeric(rt, b);
    if (nb == EXC) {
        return EXC;
    }
    if (embi_js_is_bigint(na) || embi_js_is_bigint(nb)) {
        return big_operands(rt, big_op_of(op), na, nb);
    }
    double x = embi_js_as_double(na);
    double y = embi_js_as_double(nb);
    double r;
    switch (op) {
    case EMBI_JSOP_SUB:
        r = x - y;
        break;
    case EMBI_JSOP_MUL:
        r = x * y;
        break;
    case EMBI_JSOP_DIV:
        r = x / y;
        break;
    case EMBI_JSOP_MOD:
        r = fmod(x, y);
        break;
    default:
        r = js_pow(x, y);
        break;
    }
    return embi_js_number(r);
}

static embi_js_value op_bits(embi_js_runtime *rt, uint8_t op, embi_js_value a, embi_js_value b)
{
    embi_js_value na = embi_js_to_numeric(rt, a);
    if (na == EXC) {
        return EXC;
    }
    embi_js_value nb = embi_js_to_numeric(rt, b);
    if (nb == EXC) {
        return EXC;
    }
    if (embi_js_is_bigint(na) || embi_js_is_bigint(nb)) {
        return big_operands(rt, big_op_of(op), na, nb);
    }
    if (op == EMBI_JSOP_SHR) {
        uint32_t x;
        int32_t s;
        if (!embi_js_to_uint32(rt, na, &x) || !embi_js_to_int32(rt, nb, &s)) {
            return EXC;
        }
        return embi_js_number((double)(x >> ((uint32_t)s & 31u)));
    }
    int32_t x;
    int32_t y;
    if (!embi_js_to_int32(rt, na, &x) || !embi_js_to_int32(rt, nb, &y)) {
        return EXC;
    }
    switch (op) {
    case EMBI_JSOP_BAND:
        return embi_js_from_int32(x & y);
    case EMBI_JSOP_BOR:
        return embi_js_from_int32(x | y);
    case EMBI_JSOP_BXOR:
        return embi_js_from_int32(x ^ y);
    case EMBI_JSOP_SHL:
        return embi_js_from_int32((int32_t)((uint32_t)x << ((uint32_t)y & 31u)));
    default:
        return embi_js_from_int32(x >> ((uint32_t)y & 31u));
    }
}

/* Relational operators: returns TRUE, FALSE, or EXC. */
static embi_js_value op_compare(embi_js_runtime *rt, uint8_t op, embi_js_value a, embi_js_value b)
{
    int r;
    switch (op) {
    case EMBI_JSOP_LT:
        r = embi_js_less_than(rt, a, b, true);
        if (r == EMBI_JS_CMP_THREW) {
            return EXC;
        }
        return embi_js_from_bool(r == EMBI_JS_CMP_TRUE);
    case EMBI_JSOP_GT:
        r = embi_js_less_than(rt, b, a, false);
        if (r == EMBI_JS_CMP_THREW) {
            return EXC;
        }
        return embi_js_from_bool(r == EMBI_JS_CMP_TRUE);
    case EMBI_JSOP_LE:
        r = embi_js_less_than(rt, b, a, false);
        if (r == EMBI_JS_CMP_THREW) {
            return EXC;
        }
        return embi_js_from_bool(r == EMBI_JS_CMP_FALSE);
    default:
        r = embi_js_less_than(rt, a, b, true);
        if (r == EMBI_JS_CMP_THREW) {
            return EXC;
        }
        return embi_js_from_bool(r == EMBI_JS_CMP_FALSE);
    }
}

static embi_js_value op_in(embi_js_runtime *rt, embi_js_value key, embi_js_value obj)
{
    if (!embi_js_is_object(obj)) {
        return embi_js_throw_type_error(rt, "Cannot use 'in' operator to search in a non-object");
    }
    embi_js_key k = embi_js_to_property_key(rt, key);
    if (k == EXC) {
        return EXC;
    }
    int has = embi_js_obj_has_property(rt, embi_js_as_object(obj), k);
    if (has == EMBI_JS_THREW) {
        return EXC;
    }
    return embi_js_from_bool(has == EMBI_JS_YES);
}

/* ------------------------------------------------------------------ for-in */

/* The for-in key cache (J5m2). A receiver whose prototype chain is shape-only (embi_js_obj_shape_only
 * on each object, at most ENUM_CACHE_CHAIN objects) has the same enumerable string keys for as long
 * as every object on the chain keeps its shape: the shape holds the keys, their order and their
 * attributes, and an object without elements has no index keys. The key list is stored on the
 * receiver's shape as the varray [n, shape_1 .. shape_n, key ...]. Deleted keys are still checked
 * one by one by FOR_IN_NEXT, as before. */
#define ENUM_CACHE_CHAIN 8u

/* Whether o and its prototypes can be described by their shapes alone. */
static bool enum_chain_ok(const embi_js_object *o)
{
    const embi_js_object *c = o;
    for (uint32_t depth = 1;; depth++) {
        if (depth > ENUM_CACHE_CHAIN || !embi_js_obj_shape_only(c)) {
            return false;
        }
        embi_js_value p = c->shape->proto;
        if (embi_js_is_null(p)) {
            return true;
        }
        if (!embi_js_is_object(p)) {
            return false;
        }
        c = embi_js_as_object(p);
    }
}

/* Whether the cached chain shapes of c are the chain of o now. */
static bool enum_cache_valid(const embi_js_object *o, const embi_js_varray *c)
{
    uint32_t n = (uint32_t)embi_js_as_int32(c->elems[0]);
    const embi_js_object *cur = o;
    for (uint32_t i = 0; i < n; i++) {
        if (!cur || !embi_js_obj_shape_only(cur) ||
            embi_js_as_cell(c->elems[1u + i]) != &cur->shape->hdr) {
            return false;
        }
        embi_js_value p = cur->shape->proto;
        if (i + 1u == n) {
            return embi_js_is_null(p);
        }
        if (!embi_js_is_object(p)) {
            return false;
        }
        cur = embi_js_as_object(p);
    }
    return false;
}

/* Stores the key list of o's chain on o's shape. Requires enum_chain_ok(o). */
static void enum_cache_store(embi_js_runtime *rt, embi_js_object *o, const embi_js_key_vec *keys)
{
    uint32_t n = 0;
    for (const embi_js_object *c = o;; n++) {
        embi_js_value p = c->shape->proto;
        if (embi_js_is_null(p)) {
            n++;
            break;
        }
        c = embi_js_as_object(p);
    }
    embi_js_varray *cache = embi_js_varray_new(rt, 1u + n + keys->len);
    EMBI_JS_STORE(cache, cache->elems[0], embi_js_from_int32((int32_t)n));
    const embi_js_object *c = o;
    for (uint32_t i = 0; i < n; i++) {
        EMBI_JS_STORE(cache, cache->elems[1u + i], embi_js_from_cell(&c->shape->hdr));
        if (!embi_js_is_null(c->shape->proto)) {
            c = embi_js_as_object(c->shape->proto);
        }
    }
    for (uint32_t i = 0; i < keys->len; i++) {
        EMBI_JS_STORE(cache, cache->elems[1u + n + i], embi_js_key_to_value(rt, keys->data[i]));
    }
    EMBI_JS_STORE(o->shape, o->shape->enum_cache, embi_js_from_cell(cache));
}

/* The iterator over a cached key list (J5m2): [object, next index, key...]. */
static embi_js_value enum_iter_from_cache(embi_js_runtime *rt, embi_js_object *o,
                                          const embi_js_varray *c)
{
    uint32_t n = (uint32_t)embi_js_as_int32(c->elems[0]);
    uint32_t nk = c->hdr.aux - (n + 1u);
    embi_js_varray *it = embi_js_varray_new(rt, 2u + nk);
    EMBI_JS_STORE(it, it->elems[0], embi_js_from_object(o));
    EMBI_JS_STORE(it, it->elems[1], embi_js_from_int32(0));
    for (uint32_t i = 0; i < nk; i++) {
        EMBI_JS_STORE(it, it->elems[2u + i], c->elems[1u + n + i]);
    }
    return embi_js_from_cell(it);
}

/* The iterator is an internal array: [object, next index, key...]. */
static embi_js_value for_in_init(embi_js_runtime *rt, embi_js_value v)
{
    if (embi_js_is_nullish(v)) {
        embi_js_varray *it = embi_js_varray_new(rt, 2);
        EMBI_JS_STORE(it, it->elems[0], UNDEF);
        EMBI_JS_STORE(it, it->elems[1], embi_js_from_int32(0));
        return embi_js_from_cell(it);
    }
    embi_js_object *o = embi_js_to_object(rt, v);
    if (!o) {
        return EXC;
    }
    bool cacheable = enum_chain_ok(o);
    if (cacheable && embi_js_is_cell(o->shape->enum_cache)) {
        const embi_js_varray *c = (const embi_js_varray *)embi_js_as_cell(o->shape->enum_cache);
        if (enum_cache_valid(o, c)) {
            return enum_iter_from_cache(rt, o, c);
        }
    }
    embi_js_key_vec keys = {nullptr, 0, 0};
    embi_hashmap seen;
    embi_hashmap_init(&seen);
    embi_js_object *cur = o;
    embi_js_value result = EXC;
    for (;;) {
        embi_js_key_vec own = {nullptr, 0, 0};
        if (embi_js_obj_own_keys(rt, cur, &own) != EMBI_JS_YES) {
            embi_vec_free(&own);
            goto done;
        }
        for (uint32_t i = 0; i < own.len; i++) {
            embi_js_key k = own.data[i];
            if (embi_js_is_symbol(k)) {
                continue;
            }
            uint64_t seen_val = 0;
            if (embi_hashmap_get(&seen, k, &seen_val)) {
                continue;
            }
            embi_hashmap_put(&seen, k, 1);
            embi_js_pdesc d;
            int r = embi_js_obj_get_own_property(rt, cur, k, &d);
            if (r == EMBI_JS_THREW) {
                embi_vec_free(&own);
                goto done;
            }
            if (r == EMBI_JS_YES && (d.attrs & EMBI_JS_ATTR_ENUMERABLE)) {
                embi_vec_push(&keys, k);
            }
        }
        embi_vec_free(&own);
        embi_js_value p;
        if (embi_js_obj_get_prototype_of(rt, cur, &p) == EMBI_JS_THREW) {
            goto done;
        }
        if (!embi_js_is_object(p)) {
            break;
        }
        cur = embi_js_as_object(p);
    }
    embi_js_varray *it = embi_js_varray_new(rt, 2 + keys.len);
    EMBI_JS_STORE(it, it->elems[0], embi_js_from_object(o));
    EMBI_JS_WB(it, it->elems[0]);
    EMBI_JS_STORE(it, it->elems[1], embi_js_from_int32(0));
    for (uint32_t i = 0; i < keys.len; i++) {
        embi_js_value kv = embi_js_key_to_value(rt, keys.data[i]);
        EMBI_JS_STORE(it, it->elems[2 + i], kv);
        EMBI_JS_WB(it, kv);
    }
    result = embi_js_from_cell(it);
    if (cacheable) {
        enum_cache_store(rt, o, &keys);
    }
done:
    embi_vec_free(&keys);
    embi_hashmap_free(&seen);
    return result;
}

/* ------------------------------------------------------------------ J2a helpers */

static embi_js_value tdz_error(embi_js_runtime *rt, embi_js_value name)
{
    return throw_name(rt, EMBI_JS_PROTO_REFERENCE_ERROR, "Cannot access '", name,
                      "' before initialization");
}

#define SPREAD_MAX (1u << 24)

static void spread_free(embi_js_value *buf, uint32_t n)
{
    embi_free(buf, (size_t)(n ? n : 1u) * sizeof(embi_js_value));
}

/* The array index of a key value that is an int32 or an integral double below 2^32 - 1 (J5e fast
 * paths: the key is then the index property key, and ToPropertyKey gives the same key). */
static bool index_key_u32(embi_js_value v, uint32_t *u)
{
    if (embi_js_is_int32(v)) {
        int32_t i = embi_js_as_int32(v);
        if (i < 0) {
            return false;
        }
        *u = (uint32_t)i;
        return true;
    }
    if (embi_js_is_double(v)) {
        double d = embi_js_as_double(v);
        if (d >= 0.0 && d < 4294967295.0) {
            uint32_t i = (uint32_t)d;
            if ((double)i == d) {
                *u = i;
                return true;
            }
        }
    }
    return false;
}

/* The elements of an array (the arguments of a call with a spread). The buffer is released with
 * spread_free. The values stay alive through the array, which is on the operand stack. */
static bool spread_list(embi_js_runtime *rt, embi_js_value arr, uint32_t *n, embi_js_value **out)
{
    embi_js_object *a = embi_js_as_object(arr);
    double len = 0;
    *out = nullptr;
    *n = 0;
    if (!embi_js_length_of_array_like(rt, a, &len)) {
        return false;
    }
    if (len > (double)SPREAD_MAX) {
        (void)embi_js_throw_range_error(rt, "too many arguments in spread");
        return false;
    }
    uint32_t count = (uint32_t)len;
    embi_js_value *buf = embi_calloc(count ? count : 1u, sizeof(embi_js_value));
    for (uint32_t i = 0; i < count; i++) {
        embi_js_value v;
        if (!embi_js_obj_index_get_fast(rt, a, i, &v)) {
            v = embi_js_obj_get(rt, a, embi_js_key_from_index(i), arr);
        }
        if (v == EXC) {
            spread_free(buf, count);
            return false;
        }
        buf[i] = v;
    }
    *n = count;
    *out = buf;
    return true;
}

/* Iterator records (defined with the iterator protocol below). */
static embi_js_value iter_init(embi_js_runtime *rt, embi_js_value v, bool fast);
static embi_js_varray *ir_of(embi_js_value v);
static int ir_step(embi_js_runtime *rt, embi_js_varray *r, embi_js_value *out);

/* Appends every value of src to the array a (the spread element of an array literal). The values
 * come from the iterator protocol (GetIterator, IteratorStepValue), with the fast records of
 * iter_init for unmodified arrays, strings, Maps and Sets. */
static bool array_spread(embi_js_runtime *rt, embi_js_value a_v, embi_js_value src)
{
    embi_js_object *a = embi_js_as_object(a_v);
    embi_js_value rec = iter_init(rt, src, true);
    if (rec == EXC) {
        return false;
    }
    embi_js_varray *r = ir_of(rec);
    for (;;) {
        embi_js_value v = UNDEF;
        int st = ir_step(rt, r, &v);
        if (st < 0) {
            return false;
        }
        if (st == 0) {
            return true;
        }
        if (embi_js_array_push_fast(rt, a, v)) {
            continue;
        }
        double len = 0;
        if (!embi_js_length_of_array_like(rt, a, &len)) {
            return false;
        }
        if (embi_js_create_data_property(rt, a, embi_js_key_from_index((uint32_t)len), v) ==
            EMBI_JS_THREW) {
            return false;
        }
    }
}

/* CopyDataProperties: the own enumerable properties of src (strings and symbols) are defined on
 * target, in key order. */
static bool copy_data_props(embi_js_runtime *rt, embi_js_object *target, embi_js_value src)
{
    if (embi_js_is_nullish(src)) {
        return true;
    }
    embi_js_object *from = embi_js_to_object(rt, src);
    if (!from) {
        return false;
    }
    embi_js_key_vec keys = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, from, &keys) == EMBI_JS_THREW) {
        embi_vec_free(&keys);
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < keys.len; i++) {
        embi_js_key k = keys.data[i];
        embi_js_pdesc d;
        int st = embi_js_obj_get_own_property(rt, from, k, &d);
        if (st == EMBI_JS_THREW) {
            ok = false;
        } else if (st == EMBI_JS_YES && (d.attrs & ATTR_E)) {
            embi_js_value v = embi_js_obj_get(rt, from, k, embi_js_from_object(from));
            if (v == EXC) {
                ok = false;
            } else if (embi_js_create_data_property(rt, target, k, v) == EMBI_JS_THREW) {
                ok = false;
            }
        }
    }
    embi_vec_free(&keys);
    return ok;
}

/* ------------------------------------------------------------------ J2b: destructuring */

/* An iterator record: a varray [iterator, next method, flags], kept in a stack value or a compiler
 * temporary. Flags:
 * IR_DONE once the iterator is exhausted or threw (IteratorClose is then skipped).
 * The fast records (J2c) replace the protocol when the iterator is one the spec defines and that
 * is still unmodified: nothing observable is skipped, the steps do what the intrinsic next does.
 *   IR_ARRAY: [array, next index]. The %ArrayIteratorPrototype%.next and Array.prototype[@@iterator]
 *     are the intrinsics. Each step reads length and the element through the ordinary Get.
 *   IR_STR: [string, next index]. String.prototype[@@iterator] and its next are the intrinsics.
 *   IR_COLL: [Map or Set iterator, undefined]. The entries or values iterator, and its next is the
 *     intrinsic. Steps go through embi_js_coll_iter_step.
 * IR_ARRAY and IR_STR have no iterator object, so their close looks return up on the iterator
 * prototype (where the spec's lookup would end) and, when it is defined, makes a real iterator
 * first (ir_fast_real). IR_COLL holds the real iterator, so it closes like any record. */
#define IR_DONE 1
#define IR_ARRAY 2
#define IR_STR 4
#define IR_COLL 8
#define IR_FAST (IR_ARRAY | IR_STR | IR_COLL)

static embi_js_varray *ir_of(embi_js_value v)
{
    return (embi_js_varray *)embi_js_as_cell(v);
}

static int32_t ir_flags(const embi_js_varray *r)
{
    return embi_js_as_int32(r->elems[2]);
}

static void ir_set_flags(embi_js_varray *r, int32_t flags)
{
    EMBI_JS_STORE(r, r->elems[2], embi_js_from_int32(flags));
}

static embi_js_value ir_new(embi_js_runtime *rt, embi_js_value a, embi_js_value b, int32_t flags)
{
    embi_js_varray *r = embi_js_varray_new(rt, 3);
    EMBI_JS_STORE(r, r->elems[0], a);
    EMBI_JS_WB(r, a);
    EMBI_JS_STORE(r, r->elems[1], b);
    EMBI_JS_WB(r, b);
    EMBI_JS_STORE(r, r->elems[2], embi_js_from_int32(flags));
    return embi_js_from_cell(r);
}

/* The iterator-protocol keys, interned once per runtime. */
static const embi_js_vm *iter_keys(embi_js_runtime *rt)
{
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    if (!vm->iter_keys) {
        vm->k_next = embi_js_key_c(rt, "next");
        vm->k_done = embi_js_key_c(rt, "done");
        vm->k_value = embi_js_key_c(rt, "value");
        vm->k_return = embi_js_key_c(rt, "return");
        vm->k_throw = embi_js_key_c(rt, "throw");
        vm->iter_keys = true;
    }
    return vm;
}

embi_js_key embi_js_iter_key(embi_js_runtime *rt, embi_js_ikey k)
{
    const embi_js_vm *vm = iter_keys(rt);
    switch (k) {
    case EMBI_JS_IK_NEXT:
        return vm->k_next;
    case EMBI_JS_IK_DONE:
        return vm->k_done;
    case EMBI_JS_IK_VALUE:
        return vm->k_value;
    default:
        return vm->k_return;
    }
}

embi_js_string *embi_js_unit_string(embi_js_runtime *rt, uint16_t u)
{
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    if (u < 256u && vm->unit_str[u]) {
        return vm->unit_str[u];
    }
    embi_js_string *s = embi_js_str_new_utf16(rt, &u, 1);
    if (u < 256u) {
        s = embi_js_atom_intern(rt, s);
        vm->unit_str[u] = s;
    }
    return s;
}

/* GetIterator(v, sync). With fast set, the fast records above are used when they apply. */
static embi_js_value iter_init(embi_js_runtime *rt, embi_js_value v, bool fast)
{
    embi_js_value method = embi_js_get_method(rt, v, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR));
    if (method == EXC) {
        return EXC;
    }
    if (method == UNDEF) {
        return embi_js_throw_type_error(rt, "value is not iterable");
    }
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    embi_js_realm *r = vm->realm;
    if (fast && r) {
        if (embi_js_is_object(v) && method == rt->protos[EMBI_JS_PROTO_ARRAY_VALUES]) {
            if (rt->protect & EMBI_JS_PROTECT_ARRAY_ITERATOR) {
                /* Array.prototype[@@iterator] and %ArrayIteratorPrototype%.next are intact (J5e). */
                return ir_new(rt, v, embi_js_from_int32(0), IR_ARRAY);
            }
            embi_js_value nx = embi_js_get(rt, embi_js_as_object(rt->protos[EMBI_JS_PROTO_ARRAY_ITERATOR]),
                                           iter_keys(rt)->k_next);
            if (nx == EXC) {
                return EXC;
            }
            if (nx == rt->protos[EMBI_JS_PROTO_ARRAY_ITER_NEXT]) {
                return ir_new(rt, v, embi_js_from_int32(0), IR_ARRAY);
            }
        }
        if (embi_js_is_string(v) && method == rt->protos[EMBI_JS_PROTO_STRING_ITER]) {
            embi_js_value nx = embi_js_get(rt, embi_js_as_object(rt->protos[EMBI_JS_PROTO_STRING_ITERATOR]),
                                           iter_keys(rt)->k_next);
            if (nx == EXC) {
                return EXC;
            }
            if (nx == rt->protos[EMBI_JS_PROTO_STRING_ITER_NEXT]) {
                return ir_new(rt, v, embi_js_from_int32(0), IR_STR);
            }
        }
        if (embi_js_is_object(v) && (embi_js_coll_is_map(v) || embi_js_coll_is_set(v))) {
            bool map = embi_js_coll_is_map(v);
            embi_js_value want = embi_js_coll_intrinsic(r, map ? EMBI_JS_COLL_MAP_ENTRIES
                                                               : EMBI_JS_COLL_SET_VALUES);
            if (method == want) {
                embi_js_value it = embi_js_call(rt, method, v, 0, nullptr);
                if (it == EXC) {
                    return EXC;
                }
                if (!embi_js_is_object(it)) {
                    return embi_js_throw_type_error(rt, "Result of the Symbol.iterator method is not an object");
                }
                embi_js_value nx = embi_js_get(rt, embi_js_as_object(it), iter_keys(rt)->k_next);
                if (nx == EXC) {
                    return EXC;
                }
                if (nx == embi_js_coll_intrinsic(r, map ? EMBI_JS_COLL_MAP_ITER_NEXT
                                                        : EMBI_JS_COLL_SET_ITER_NEXT)) {
                    return ir_new(rt, it, UNDEF, IR_COLL);
                }
                return ir_new(rt, it, nx, 0);
            }
        }
    }
    embi_js_value it = embi_js_call(rt, method, v, 0, nullptr);
    if (it == EXC) {
        return EXC;
    }
    if (!embi_js_is_object(it)) {
        return embi_js_throw_type_error(rt, "Result of the Symbol.iterator method is not an object");
    }
    embi_js_value next = embi_js_get(rt, embi_js_as_object(it), iter_keys(rt)->k_next);
    if (next == EXC) {
        return EXC;
    }
    return ir_new(rt, it, next, 0);
}

/* IteratorStepValue on a record: 1 with the value in *out, 0 when done (or already done), -1 when
 * it threw. A throw marks the record done, so no IteratorClose follows it. */
static int ir_step(embi_js_runtime *rt, embi_js_varray *r, embi_js_value *out)
{
    int32_t flags = ir_flags(r);
    *out = UNDEF;
    if (flags & IR_DONE) {
        return 0;
    }
    if (flags & IR_COLL) {
        int st = embi_js_coll_iter_step(rt, embi_js_as_object(r->elems[0]), out);
        if (st == EMBI_JS_YES) {
            return 1;
        }
        ir_set_flags(r, flags | IR_DONE);
        return st == EMBI_JS_NO ? 0 : -1;
    }
    if (flags & IR_STR) {
        embi_js_string *str = embi_js_str_flat(rt, r->elems[0]);
        uint32_t i = (uint32_t)embi_js_as_int32(r->elems[1]);
        uint32_t n = embi_js_str_len(str);
        if (i >= n) {
            ir_set_flags(r, flags | IR_DONE);
            return 0;
        }
        uint16_t units[2];
        uint32_t w = 1;
        units[0] = embi_js_str_at(str, i);
        if (units[0] >= 0xD800u && units[0] <= 0xDBFFu && i + 1u < n) {
            uint16_t lo = embi_js_str_at(str, i + 1u);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                units[1] = lo;
                w = 2;
            }
        }
        embi_js_string *cp = w == 1 ? embi_js_unit_string(rt, units[0])
                                    : embi_js_str_new_utf16(rt, units, w);
        if (!cp) {
            ir_set_flags(r, flags | IR_DONE);
            return -1;
        }
        EMBI_JS_STORE(r, r->elems[1], embi_js_from_int32((int32_t)(i + w)));
        EMBI_JS_WB(r, r->elems[1]);
        *out = embi_js_from_string(cp);
        return 1;
    }
    if (flags & IR_ARRAY) {
        embi_js_object *a = embi_js_as_object(r->elems[0]);
        double len = 0;
        if (!embi_js_length_of_array_like(rt, a, &len)) {
            ir_set_flags(r, flags | IR_DONE);
            return -1;
        }
        double idx = embi_js_is_int32(r->elems[1]) ? (double)embi_js_as_int32(r->elems[1])
                                                   : embi_js_as_double(r->elems[1]);
        if (idx >= len) {
            ir_set_flags(r, flags | IR_DONE);
            return 0;
        }
        uint32_t i = (uint32_t)idx; /* an array's length is below 2^32 */
        EMBI_JS_STORE(r, r->elems[1], embi_js_from_double(idx + 1.0));
        embi_js_value v;
        if (!embi_js_obj_index_get_fast(rt, a, i, &v)) {
            v = embi_js_obj_get(rt, a, embi_js_key_from_index(i), r->elems[0]);
        }
        if (v == EXC) {
            ir_set_flags(r, flags | IR_DONE);
            return -1;
        }
        *out = v;
        return 1;
    }
    embi_js_value res = embi_js_call(rt, r->elems[1], r->elems[0], 0, nullptr);
    if (res == EXC) {
        ir_set_flags(r, flags | IR_DONE);
        return -1;
    }
    if (!embi_js_is_object(res)) {
        ir_set_flags(r, flags | IR_DONE);
        (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
        return -1;
    }
    embi_js_value d = embi_js_get(rt, embi_js_as_object(res), iter_keys(rt)->k_done);
    if (d == EXC) {
        ir_set_flags(r, flags | IR_DONE);
        return -1;
    }
    if (embi_js_to_boolean(d)) {
        ir_set_flags(r, flags | IR_DONE);
        return 0;
    }
    embi_js_value v = embi_js_get(rt, embi_js_as_object(res), iter_keys(rt)->k_value);
    if (v == EXC) {
        ir_set_flags(r, flags | IR_DONE);
        return -1;
    }
    *out = v;
    return 1;
}

/* The remaining values of a record, in a new array (the rest element of an array pattern). */
static embi_js_value iter_rest(embi_js_runtime *rt, embi_js_varray *r)
{
    embi_js_object *arr = embi_js_array_create_default(rt, 0.0);
    uint32_t n = 0;
    for (;;) {
        embi_js_value v = UNDEF;
        int st = ir_step(rt, r, &v);
        if (st < 0) {
            return EXC;
        }
        if (st == 0) {
            break;
        }
        if (embi_js_create_data_property(rt, arr, embi_js_key_from_index(n), v) == EMBI_JS_THREW) {
            ir_set_flags(r, ir_flags(r) | IR_DONE);
            return EXC;
        }
        n++;
    }
    return embi_js_from_object(arr);
}

/* The close of an IR_ARRAY or IR_STR record, which has no iterator object. The spec's lookup of
 * return (on the iterator, through its prototype chain) is done on the prototype. When it finds
 * nothing, the record is done without a call. When it finds a method, the record becomes the real
 * iterator at the same position (the intrinsic creates it, and its index takes the record's), so
 * the call gets the receiver the spec names. Returns 0 when there is no return, 1 when the record
 * is now generic, and -1 with the error pending. */
static int ir_fast_real(embi_js_runtime *rt, embi_js_varray *r)
{
    int32_t flags = ir_flags(r);
    bool arr = (flags & IR_ARRAY) != 0;
    embi_js_value proto =
        rt->protos[arr ? EMBI_JS_PROTO_ARRAY_ITERATOR : EMBI_JS_PROTO_STRING_ITERATOR];
    embi_js_value ret = embi_js_get_method(rt, proto, iter_keys(rt)->k_return);
    if (ret == EXC) {
        return -1;
    }
    if (ret == UNDEF) {
        return 0;
    }
    embi_js_value make = rt->protos[arr ? EMBI_JS_PROTO_ARRAY_VALUES : EMBI_JS_PROTO_STRING_ITER];
    embi_js_value it = embi_js_call(rt, make, r->elems[0], 0, nullptr);
    if (it == EXC) {
        return -1;
    }
    /* Both intrinsic iterators keep their next index in state slot 1 (see builtins_array.c and
     * builtins_string.c). */
    embi_js_varray *st = embi_js_as_object(it)->internal;
    EMBI_JS_STORE(st, st->elems[1], r->elems[1]);
    EMBI_JS_WB(st, st->elems[1]);
    EMBI_JS_STORE(r, r->elems[0], it);
    EMBI_JS_WB(r, it);
    EMBI_JS_STORE(r, r->elems[1], UNDEF);
    ir_set_flags(r, 0);
    return 1;
}

/* IteratorClose on a normal completion: the errors of return propagate. */
static bool ir_close(embi_js_runtime *rt, embi_js_varray *r)
{
    int32_t flags = ir_flags(r);
    if (flags & IR_DONE) {
        return true;
    }
    if (flags & (IR_ARRAY | IR_STR)) {
        int m = ir_fast_real(rt, r);
        if (m < 0) {
            ir_set_flags(r, ir_flags(r) | IR_DONE);
            return false;
        }
        if (m == 0) {
            ir_set_flags(r, flags | IR_DONE);
            return true;
        }
        flags = ir_flags(r);
    }
    ir_set_flags(r, flags | IR_DONE);
    embi_js_value ret = embi_js_get_method(rt, r->elems[0], iter_keys(rt)->k_return);
    if (ret == EXC) {
        return false;
    }
    if (ret == UNDEF) {
        return true;
    }
    embi_js_value res = embi_js_call(rt, ret, r->elems[0], 0, nullptr);
    if (res == EXC) {
        return false;
    }
    if (!embi_js_is_object(res)) {
        (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
        return false;
    }
    return true;
}

/* IteratorClose on a throw completion: the errors of return are dropped, and the caller rethrows
 * the original exception, which it holds on the operand stack. */
static void ir_close_throw(embi_js_runtime *rt, embi_js_varray *r)
{
    int32_t flags = ir_flags(r);
    if (flags & IR_DONE) {
        return;
    }
    if (flags & (IR_ARRAY | IR_STR)) {
        int m = ir_fast_real(rt, r);
        if (m < 0) {
            embi_js_exception_clear(rt);
            ir_set_flags(r, ir_flags(r) | IR_DONE);
            return;
        }
        if (m == 0) {
            ir_set_flags(r, flags | IR_DONE);
            return;
        }
        flags = ir_flags(r);
    }
    ir_set_flags(r, flags | IR_DONE);
    embi_js_value ret = embi_js_get_method(rt, r->elems[0], iter_keys(rt)->k_return);
    if (ret == EXC) {
        embi_js_exception_clear(rt);
        return;
    }
    if (ret == UNDEF) {
        return;
    }
    if (embi_js_call(rt, ret, r->elems[0], 0, nullptr) == EXC) {
        embi_js_exception_clear(rt);
    }
}

/* CopyDataProperties(target, src, excl): like copy_data_props, skipping the keys in excl (an array
 * of values, canonicalized to property keys here). */
static bool copy_rest(embi_js_runtime *rt, embi_js_object *target, embi_js_value src,
                      embi_js_value excl)
{
    embi_js_object *from = embi_js_to_object(rt, src);
    if (!from) {
        return false;
    }
    embi_js_object *ex = embi_js_as_object(excl);
    double n = 0;
    if (!embi_js_length_of_array_like(rt, ex, &n)) {
        return false;
    }
    embi_js_key_vec skip = {nullptr, 0, 0};
    for (uint32_t i = 0; (double)i < n; i++) {
        embi_js_value v = embi_js_obj_get(rt, ex, embi_js_key_from_index(i), excl);
        if (v == EXC) {
            embi_vec_free(&skip);
            return false;
        }
        embi_js_key k = embi_js_to_property_key(rt, v);
        if (k == EXC) {
            embi_vec_free(&skip);
            return false;
        }
        embi_vec_push(&skip, k);
    }
    embi_js_key_vec keys = {nullptr, 0, 0};
    if (embi_js_obj_own_keys(rt, from, &keys) == EMBI_JS_THREW) {
        embi_vec_free(&keys);
        embi_vec_free(&skip);
        return false;
    }
    bool ok = true;
    for (uint32_t i = 0; ok && i < keys.len; i++) {
        embi_js_key k = keys.data[i];
        bool excluded = false;
        for (uint32_t j = 0; !excluded && j < skip.len; j++) {
            excluded = skip.data[j] == k;
        }
        if (excluded) {
            continue;
        }
        embi_js_pdesc d;
        int st = embi_js_obj_get_own_property(rt, from, k, &d);
        if (st == EMBI_JS_THREW) {
            ok = false;
        } else if (st == EMBI_JS_YES && (d.attrs & ATTR_E)) {
            embi_js_value v = embi_js_obj_get(rt, from, k, embi_js_from_object(from));
            if (v == EXC) {
                ok = false;
            } else if (embi_js_create_data_property(rt, target, k, v) == EMBI_JS_THREW) {
                ok = false;
            }
        }
    }
    embi_vec_free(&keys);
    embi_vec_free(&skip);
    return ok;
}

/* The name of a function defined under a computed key: the key as a string, a symbol's
 * description in brackets, with a "get " or "set " prefix for accessors. */
static embi_js_string *key_fn_name(embi_js_runtime *rt, embi_js_value key, uint32_t kind)
{
    embi_js_string *base = nullptr;
    if (embi_js_is_symbol(key)) {
        embi_js_value d = ((embi_js_symbol *)embi_js_as_cell(key))->description;
        if (embi_js_is_string(d)) {
            embi_js_string *open = embi_js_str_new_utf8(rt, "[", 1);
            embi_js_string *close = embi_js_str_new_utf8(rt, "]", 1);
            if (!open || !close) {
                return nullptr;
            }
            embi_js_string *t = embi_js_str_concat(rt, open, embi_js_as_string(d));
            base = t ? embi_js_str_concat(rt, t, close) : nullptr;
        } else {
            base = embi_js_str_new_utf8(rt, "", 0);
        }
    } else {
        base = embi_js_to_string(rt, key);
    }
    if (!base) {
        return nullptr;
    }
    if (kind == 0) {
        return base;
    }
    embi_js_string *prefix = embi_js_str_new_utf8(rt, kind == 1 ? "get " : "set ", 4);
    return prefix ? embi_js_str_concat(rt, prefix, base) : nullptr;
}

/* ------------------------------------------------------------------ classes (J2d) */

/* The closure data of fn when fn is a script function, else nullptr. */
static embi_js_closure *closure_or_null(embi_js_value fn)
{
    if (embi_js_is_object(fn) && embi_js_as_object(fn)->klass == &embi_js_class_closure) {
        return closure_of(embi_js_as_object(fn));
    }
    return nullptr;
}

/* InitializeInstanceElements: the private installer, then the field initializer, of the class
 * constructor f run on self. */
static bool class_fields_init(embi_js_runtime *rt, embi_js_value f, embi_js_value self)
{
    embi_js_closure *cl = closure_or_null(f);
    if (!cl) {
        return true;
    }
    if (embi_js_is_object(cl_pinit(cl)) && embi_js_call(rt, cl_pinit(cl), self, 0, nullptr) == EXC) {
        return false;
    }
    if (embi_js_is_object(cl_finit(cl)) && embi_js_call(rt, cl_finit(cl), self, 0, nullptr) == EXC) {
        return false;
    }
    return true;
}

/* The object that super property references of home look in: home.[[GetPrototypeOf]](). */
static embi_js_value super_proto(embi_js_runtime *rt, embi_js_value home)
{
    embi_js_value p = UNDEF;
    if (!embi_js_is_object(home)) {
        return embi_js_throw_type_error(rt, "'super' keyword unexpected here");
    }
    if (embi_js_obj_get_prototype_of(rt, embi_js_as_object(home), &p) == EMBI_JS_THREW) {
        return EXC;
    }
    if (!embi_js_is_object(p)) {
        return embi_js_throw_type_error(rt, "Cannot read properties of null (reading through super)");
    }
    return p;
}

/* The description (the "#name" text) of a private name. */
static embi_js_value priv_text(embi_js_value k)
{
    return ((embi_js_symbol *)embi_js_as_cell(k))->description;
}

/* The own private element k of v, when v is an object that has one. */
static bool priv_find(embi_js_value v, embi_js_value k, embi_js_pdesc *d)
{
    return embi_js_is_object(v) && embi_js_priv_get_own(embi_js_as_object(v), k, d);
}

static embi_js_value priv_read(embi_js_runtime *rt, embi_js_value obj, embi_js_value k,
                               const embi_js_pdesc *d)
{
    if (d->has & EMBI_JS_PD_GET) {
        if (!embi_js_is_object(d->get)) {
            return throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "'", priv_text(k),
                              "' was defined without a getter");
        }
        return embi_js_call(rt, d->get, obj, 0, nullptr);
    }
    return d->value;
}

/* PrivateGet: the value, or EXC. */
static embi_js_value priv_get(embi_js_runtime *rt, embi_js_value obj, embi_js_value k)
{
    embi_js_pdesc d;
    memset(&d, 0, sizeof d);
    if (!priv_find(obj, k, &d)) {
        return throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR,
                          "Cannot read private member ", priv_text(k),
                          " from an object whose class did not declare it");
    }
    return priv_read(rt, obj, k, &d);
}

/* PrivateSet: UNDEF on success, EXC otherwise. */
static embi_js_value priv_write(embi_js_runtime *rt, embi_js_value obj, embi_js_value k,
                                embi_js_value v)
{
    embi_js_pdesc d;
    memset(&d, 0, sizeof d);
    if (!priv_find(obj, k, &d)) {
        return throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR,
                          "Cannot write private member ", priv_text(k),
                          " to an object whose class did not declare it");
    }
    if (d.has & EMBI_JS_PD_GET) {
        if (!embi_js_is_object(d.set)) {
            return throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "'", priv_text(k),
                              "' was defined without a setter");
        }
        return embi_js_call(rt, d.set, obj, 1, &v) == EXC ? EXC : UNDEF;
    }
    if (!(d.attrs & EMBI_JS_ATTR_WRITABLE)) {
        return embi_js_throw_type_error(rt, "Private method is not writable");
    }
    embi_js_define_raw(rt, embi_js_as_object(obj), k, v, EMBI_JS_ATTR_WRITABLE);
    return UNDEF;
}

/* A private element is added once per object: returns EXC when k is already on obj. */
static embi_js_value priv_add_check(embi_js_runtime *rt, embi_js_value obj, embi_js_value k)
{
    embi_js_pdesc d;
    memset(&d, 0, sizeof d);
    if (priv_find(obj, k, &d)) {
        return throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot initialize ", priv_text(k),
                          " twice on the same object");
    }
    /* PrivateFieldAdd, PrivateMethodOrAccessorAdd: a non-extensible object takes no new element. */
    if (embi_js_is_object(obj) && !embi_js_obj_is_extensible(embi_js_as_object(obj))) {
        return throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot define ", priv_text(k),
                          " on a non-extensible object");
    }
    return UNDEF;
}

/* The name of a function defined by a class or object literal member, from its key. */
static embi_js_value set_method_name(embi_js_runtime *rt, embi_js_value fnv, embi_js_value key,
                                     uint32_t kind)
{
    embi_js_string *name = key_fn_name(rt, key, kind);
    if (!name) {
        return EXC;
    }
    embi_js_define_raw(rt, embi_js_as_object(fnv), embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(name), ATTR_C);
    return UNDEF;
}

/* ------------------------------------------------------------------ closures and natives */

static void closure_trace(embi_js_object *o, embi_js_tracer *t)
{
    embi_js_closure *cl = closure_of(o);
    if (!cl) {
        return;
    }
    embi_js_mark_value(t, cl->code);
    if (cl->has_ext) {
        embi_js_mark_value(t, cl_ext(cl)->home);
        embi_js_mark_value(t, cl_ext(cl)->pinit);
        embi_js_mark_value(t, cl_ext(cl)->finit);
    }
    for (uint32_t i = 0; i < cl->nupvals; i++) {
        embi_js_mark_value(t, cl->upvals[i]);
    }
    t->external += closure_bytes(cl->nupvals, cl->has_ext != 0);
}

static void closure_finalize(embi_js_object *o)
{
    embi_js_closure *cl = closure_of(o);
    if (!cl) {
        return;
    }
    embi_free(cl, closure_bytes(cl->nupvals, cl->has_ext != 0));
    o->internal = nullptr;
}

static embi_js_value closure_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                  int argc, const embi_js_value *argv);
static embi_js_value closure_call_nt(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                     int argc, const embi_js_value *argv, embi_js_value new_target);
static embi_js_value closure_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                       const embi_js_value *argv, embi_js_value new_target);

const embi_js_class embi_js_class_closure = {
    .name = "Function",
    .flags = EMBI_JS_CLASS_IC,
    .call = closure_call,
    .construct = closure_construct,
    .trace = closure_trace,
    .finalize = closure_finalize,
};

static void native_finalize(embi_js_object *o)
{
    if (o->internal) {
        embi_free(o->internal, sizeof(embi_js_native_data));
        o->internal = nullptr;
    }
}

static embi_js_value native_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv);
static embi_js_value native_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                      const embi_js_value *argv, embi_js_value new_target);

const embi_js_class embi_js_class_native = {
    .name = "Function",
    .flags = EMBI_JS_CLASS_IC,
    .call = native_call,
    .construct = native_construct,
    .finalize = native_finalize,
};

static embi_js_object *make_closure(embi_js_runtime *rt, embi_js_value code_v, embi_js_value *slots,
                                    const embi_js_object *parent)
{
    embi_js_code *code = code_of(code_v);
    bool gen = (code->flags & EMBI_JS_CODE_GENERATOR) != 0;
    bool async = (code->flags & EMBI_JS_CODE_ASYNC) != 0;
    enum embi_js_proto fproto = EMBI_JS_PROTO_FUNCTION;
    if (async) {
        fproto = gen ? EMBI_JS_PROTO_ASYNC_GENERATOR_FUNCTION : EMBI_JS_PROTO_ASYNC_FUNCTION;
    } else if (gen) {
        fproto = EMBI_JS_PROTO_GENERATOR_FUNCTION;
    }
    embi_js_object *o = embi_js_object_new_class(rt, intrinsic(rt, fproto), &embi_js_class_closure);
    if (code->flags & EMBI_JS_CODE_CONSTRUCTOR) {
        o->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    }
    /* One block holds the closure record and its upvalue array (J5m2): the upvalues follow the
     * record, and closure_finalize frees both with the same size. */
    bool ext = (code->flags & EMBI_JS_CODE_HOMED) != 0;
    embi_js_closure *cl = embi_calloc(1, closure_bytes(code->nupv, ext));
    cl->code = code_v;
    cl->nupvals = code->nupv;
    cl->has_ext = ext ? 1u : 0u;
    if (ext) {
        embi_js_closure_ext *e = (embi_js_closure_ext *)(cl + 1);
        e->home = e->pinit = e->finit = EMBI_JS_UNDEFINED;
        cl->upvals = (embi_js_value *)(e + 1);
    } else {
        cl->upvals = (embi_js_value *)(cl + 1);
    }
    o->internal = cl;
    EMBI_JS_WB_STRUCT(o);
    for (uint32_t i = 0; i < code->nupv; i++) {
        const embi_js_upval_desc *d = &code->upv[i];
        embi_js_value uv = EMBI_JS_EMPTY;
        if (d->from == EMBI_JS_UPV_LOCAL) {
            /* A captured local lives in a box made when its scope was entered. A slot that does
             * not hold a box yet gets one now, so the closure never holds a non-cell. */
            embi_js_value cur = slots[d->index];
            if (!embi_js_is_cell(cur) || embi_js_as_cell(cur)->type != EMBI_JS_CELL_BOX) {
                embi_js_box *b = embi_js_box_new(rt, embi_js_is_empty(cur) ? UNDEF : cur);
                slots[d->index] = embi_js_from_cell(b);
                cur = slots[d->index];
            }
            uv = cur;
        } else if (d->from == EMBI_JS_UPV_PARENT) {
            uv = closure_upvals(parent)[d->index];
        }
        cl->upvals[i] = uv;
        EMBI_JS_WB(o, uv);
    }
    /* A new closure has no own property yet, so its properties are appended (J5m2). The shape they lead
     * to is the same for every closure of this code from the same start shape, so the first closure
     * records it (code->fn_from/fn_to) and later ones write the slots and switch shape at once (J5n1).
     * J5n2: the `prototype` of an ordinary function is lazy (object.h): the property exists in the shape
     * with the EMBI_JS_ATTR_LAZY flag and an EMPTY slot, and its object is made by the first access
     * that can see it. A generator's prototype object is made here as before. */
    embi_js_value vals[3];
    uint32_t nv = 0;
    vals[nv++] = embi_js_number((double)code->fn_length);
    vals[nv++] = embi_js_is_string(code->name) ? code->name : embi_js_name_val(rt, EMBI_JS_NAME_EMPTY);
    bool has_proto = gen || (code->flags & EMBI_JS_CODE_CONSTRUCTOR) != 0;
    uint8_t proto_attrs = ATTR_W;
    if (gen) {
        vals[nv++] = embi_js_from_object(embi_js_object_new(
            rt, intrinsic(rt, async ? EMBI_JS_PROTO_ASYNC_GENERATOR : EMBI_JS_PROTO_GENERATOR)));
    } else if (has_proto) {
        vals[nv++] = EMBI_JS_EMPTY;
        proto_attrs = (uint8_t)(ATTR_W | EMBI_JS_ATTR_LAZY);
        o->hdr.flags |= EMBI_JS_OBJ_LAZY_PROTO;
    }
    if (code->fn_from == o->shape && code->fn_to) {
        embi_js_obj_append_shape(rt, o, code->fn_to, vals, nv);
        return o;
    }
    embi_js_shape *from = o->shape;
    embi_js_define_fresh(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), vals[0], ATTR_C);
    embi_js_define_fresh(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_NAME), vals[1], ATTR_C);
    if (has_proto) {
        embi_js_define_fresh(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE), vals[2], proto_attrs);
    }
    if (!embi_js_obj_dict(o) && o->shape->count == from->count + nv) {
        code->fn_from = from;
        code->fn_to = o->shape;
        EMBI_JS_WB(code, embi_js_from_cell(from));
        EMBI_JS_WB(code, embi_js_from_cell(o->shape));
    }
    return o;
}

embi_js_object *embi_js_closure_from_code(embi_js_runtime *rt, embi_js_value code)
{
    return make_closure(rt, code, nullptr, nullptr);
}

static embi_js_object *native_make(embi_js_realm *r, const char *name, int length,
                                   embi_js_native fn, embi_js_native_ctor cfn, int magic,
                                   bool ctor)
{
    embi_js_runtime *rt = r->rt;
    embi_js_object *o = embi_js_object_new_class(rt, intrinsic(rt, EMBI_JS_PROTO_FUNCTION),
                                                 &embi_js_class_native);
    embi_js_native_data *nd = embi_calloc(1, sizeof *nd);
    nd->fn = fn;
    nd->cfn = cfn;
    nd->magic = magic;
    nd->realm = r;
    nd->ctor = ctor;
    o->internal = nd;
    EMBI_JS_WB_STRUCT(o);
    if (ctor) {
        o->hdr.flags |= EMBI_JS_OBJ_CONSTRUCTOR;
    }
    embi_js_define_raw(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH),
                       embi_js_number((double)length), ATTR_C);
    embi_js_define_raw(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                       embi_js_from_string(embi_js_atom_intern_utf8(rt, name, strlen(name))),
                       ATTR_C);
    return o;
}

embi_js_object *embi_js_native_function_new(embi_js_realm *r, const char *name, int length,
                                            embi_js_native fn, int magic)
{
    return native_make(r, name, length, fn, nullptr, magic, false);
}

embi_js_object *embi_js_native_constructor_new(embi_js_realm *r, const char *name, int length,
                                               embi_js_native_ctor fn, int magic)
{
    return native_make(r, name, length, nullptr, fn, magic, true);
}

void embi_js_define_method(embi_js_realm *r, embi_js_object *o, const char *name,
                           embi_js_native fn, int length, int magic)
{
    embi_js_object *f = embi_js_native_function_new(r, name, length, fn, magic);
    embi_js_define_raw(r->rt, o, embi_js_key_c(r->rt, name), embi_js_from_object(f), ATTR_W | ATTR_C);
}

static embi_js_value native_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)rt;
    embi_js_native_data *nd = f->internal;
    /* W5a0: the body reads its data through embi_js_native_data_current (restored on return). */
    void *prev = nd->realm->cur_data;
    nd->realm->cur_data = nd->data;
    embi_js_value res = nd->cfn ? nd->cfn(nd->realm, this_v, argc, argv, nd->magic, EMBI_JS_UNDEFINED)
                                : nd->fn(nd->realm, this_v, argc, argv, nd->magic);
    nd->realm->cur_data = prev;
    return res;
}

static embi_js_value native_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                      const embi_js_value *argv, embi_js_value new_target)
{
    embi_js_native_data *nd = f->internal;
    if (nd->cfn) {
        /* A native constructor makes its own object (GetPrototypeFromConstructor exactly once). */
        void *prev = nd->realm->cur_data;
        nd->realm->cur_data = nd->data;
        embi_js_value res = nd->cfn(nd->realm, EMBI_JS_UNDEFINED, argc, argv, nd->magic, new_target);
        nd->realm->cur_data = prev;
        return res;
    }
    embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_OBJECT);
    if (proto == EXC) {
        return EXC;
    }
    embi_js_object *self = embi_js_object_new(rt, proto);
    void *prev = nd->realm->cur_data;
    nd->realm->cur_data = nd->data;
    embi_js_value r = nd->fn(nd->realm, embi_js_from_object(self), argc, argv, nd->magic);
    nd->realm->cur_data = prev;
    if (r == EXC) {
        return EXC;
    }
    return embi_js_is_object(r) ? r : embi_js_from_object(self);
}

/* ------------------------------------------------------------------ arguments objects */

/* The aliasing of a sloppy arguments object. boxes[i] is the box of parameter i while index i
 * is still mapped to it, and EMPTY once it is not. */
typedef struct embi_js_args {
    embi_js_value *boxes;
    uint32_t cap;
    uint32_t n;
} embi_js_args;

static bool args_mapped(const embi_js_object *o, embi_js_key k, uint32_t *idx)
{
    const embi_js_args *st = o->internal;
    if (!st || !embi_js_key_is_index(k)) {
        return false;
    }
    uint32_t i = embi_js_key_index(k);
    if (i >= st->n || embi_js_is_empty(st->boxes[i])) {
        return false;
    }
    *idx = i;
    return true;
}

static void args_trace(embi_js_object *o, embi_js_tracer *t);
static void args_finalize(embi_js_object *o);

/* The class an arguments object has while the ordinary internal methods run on it. It has no
 * hooks, so the ordinary behavior applies, but it keeps trace and finalize: a collection during
 * the call (define can allocate) still marks the parameter boxes and releases the state. */
static const embi_js_class args_ordinary_class = {
    .name = "Arguments",
    .trace = args_trace,
    .finalize = args_finalize,
};

/* The ordinary behavior of an object of the arguments class, with the class swapped for the one
 * above during the call. */
static int args_ordinary_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                                 embi_js_pdesc *out)
{
    o->klass = &args_ordinary_class;
    int r = embi_js_obj_get_own_property(rt, o, k, out);
    o->klass = &embi_js_class_arguments;
    return r;
}

/* While a parameter is mapped its box is the authority for the value. The stored property is
 * brought up to date before an ordinary define or delete reads it. */
static void args_sync_store(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_value v)
{
    embi_js_pdesc d = {v, EMBI_JS_EMPTY, EMBI_JS_EMPTY, EMBI_JS_PD_VALUE, 0};
    o->klass = &args_ordinary_class;
    (void)embi_js_obj_define_own_property(rt, o, k, &d);
    o->klass = &embi_js_class_arguments;
}

static int args_get_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k, embi_js_pdesc *out)
{
    int r = args_ordinary_get_own(rt, o, k, out);
    uint32_t i = 0;
    if (r == EMBI_JS_YES && args_mapped(o, k, &i)) {
        const embi_js_args *st = o->internal;
        out->value = ((embi_js_box *)embi_js_as_cell(st->boxes[i]))->value;
    }
    return r;
}

static int args_define_own(embi_js_runtime *rt, embi_js_object *o, embi_js_key k,
                           const embi_js_pdesc *d)
{
    uint32_t i = 0;
    bool mapped = args_mapped(o, k, &i);
    embi_js_args *st = o->internal;
    if (mapped) {
        args_sync_store(rt, o, k, ((embi_js_box *)embi_js_as_cell(st->boxes[i]))->value);
    }
    o->klass = &args_ordinary_class;
    int r = embi_js_obj_define_own_property(rt, o, k, d);
    o->klass = &embi_js_class_arguments;
    if (r != EMBI_JS_YES || !mapped) {
        return r;
    }
    if (embi_js_pd_is_accessor(d)) {
        st->boxes[i] = EMBI_JS_EMPTY;
        return r;
    }
    embi_js_box *b = (embi_js_box *)embi_js_as_cell(st->boxes[i]);
    if (d->has & EMBI_JS_PD_VALUE) {
        b->value = d->value;
        EMBI_JS_WB(b, d->value);
    }
    if ((d->has & EMBI_JS_PD_WRITABLE) && !(d->attrs & EMBI_JS_ATTR_WRITABLE)) {
        st->boxes[i] = EMBI_JS_EMPTY;
    }
    return r;
}

static int args_delete(embi_js_runtime *rt, embi_js_object *o, embi_js_key k)
{
    uint32_t i = 0;
    bool mapped = args_mapped(o, k, &i);
    o->klass = &args_ordinary_class;
    int r = embi_js_obj_delete(rt, o, k);
    o->klass = &embi_js_class_arguments;
    if (r == EMBI_JS_YES && mapped) {
        ((embi_js_args *)o->internal)->boxes[i] = EMBI_JS_EMPTY;
    }
    return r;
}

static void args_trace(embi_js_object *o, embi_js_tracer *t)
{
    const embi_js_args *st = o->internal;
    if (!st) {
        return;
    }
    for (uint32_t i = 0; i < st->n; i++) {
        if (!embi_js_is_empty(st->boxes[i])) {
            embi_js_mark_value(t, st->boxes[i]);
        }
    }
}

static void args_finalize(embi_js_object *o)
{
    embi_js_args *st = o->internal;
    if (!st) {
        return;
    }
    embi_free(st->boxes, (size_t)st->cap * sizeof(embi_js_value));
    embi_free(st, sizeof *st);
    o->internal = nullptr;
}

const embi_js_class embi_js_class_arguments = {
    .name = "Arguments",
    .get_own = args_get_own,
    .define_own = args_define_own,
    .delete = args_delete,
    .trace = args_trace,
    .finalize = args_finalize,
};

/* Builds the arguments object of a call. base holds the frame's parameter slots, already boxed
 * where they are mapped (pmap). */
static embi_js_object *make_arguments(embi_js_runtime *rt, embi_js_object *callee,
                                      const embi_js_code *code, int argc,
                                      const embi_js_value *argv, const embi_js_value *base)
{
    embi_js_object *a = embi_js_object_new_class(rt, intrinsic(rt, EMBI_JS_PROTO_OBJECT),
                                                 &embi_js_class_arguments);
    embi_js_args *st = embi_calloc(1, sizeof *st);
    st->cap = argc > 0 ? (uint32_t)argc : 1u;
    st->boxes = embi_calloc(st->cap, sizeof(embi_js_value));
    a->internal = st;
    EMBI_JS_WB_STRUCT(a);
    for (int i = 0; i < argc; i++) {
        embi_js_create_data_property(rt, a, embi_js_key_from_index((uint32_t)i), argv[i]);
    }
    uint32_t nm = (uint32_t)argc < code->npmap ? (uint32_t)argc : code->npmap;
    for (uint32_t i = 0; i < nm; i++) {
        st->boxes[i] = code->pmap[i] ? base[i] : EMBI_JS_EMPTY;
    }
    st->n = nm;
    if ((code->flags & EMBI_JS_CODE_STRICT) || !(code->flags & EMBI_JS_CODE_MAPPED_ARGS)) {
        /* Unmapped arguments objects (strict code, non-simple parameters) get %ThrowTypeError%. */
        embi_js_object *th = rt->vm->realm ? rt->vm->realm->thrower : nullptr;
        if (th) {
            embi_js_define_accessor_raw(rt, a, embi_js_key_c(rt, "callee"), embi_js_from_object(th),
                                        embi_js_from_object(th), 0);
        }
    } else {
        embi_js_define_raw(rt, a, embi_js_key_c(rt, "callee"), embi_js_from_object(callee),
                           EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    }
    embi_js_define_raw(rt, a, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH),
                       embi_js_number((double)argc), ATTR_W | ATTR_C);
    embi_js_define_raw(rt, a, embi_js_wk_sym(rt, EMBI_JS_WK_ITERATOR),
                       rt->protos[EMBI_JS_PROTO_ARRAY_VALUES], ATTR_W | ATTR_C);
    return a;
}

/* ------------------------------------------------------------------ scope entries */

/* The value a scope entry reads: LOC holds the value in the slot, BOX holds a box in the slot,
 * UPV holds a box in an upvalue. */
static embi_js_value ent_value(const embi_js_value *slots, const embi_js_value *upv, uint8_t src,
                               uint32_t index)
{
    switch (src) {
    case EMBI_JS_SRC_LOC:
        return slots[index];
    case EMBI_JS_SRC_BOX:
        return ((embi_js_box *)embi_js_as_cell(slots[index]))->value;
    default:
        return ((embi_js_box *)embi_js_as_cell(upv[index]))->value;
    }
}

static embi_js_box *ent_box(const embi_js_value *slots, const embi_js_value *upv, uint8_t src,
                            uint32_t index)
{
    return (embi_js_box *)embi_js_as_cell(src == EMBI_JS_SRC_UPV ? upv[index] : slots[index]);
}

/* A resolved dynamic reference: a with object's property, or a box of a record. */
typedef struct dref {
    embi_js_object *obj; /* with object (the binding is its property) */
    embi_js_object *rec; /* record (the binding is the property holding box) */
    embi_js_box *box;
    embi_js_key key;
    bool deletable;      /* eval-declared variable: delete removes it */
    bool ro;             /* the record's name is not writable (an import): a store throws/ignored */
} dref;

static dref dref_new(embi_js_runtime *rt, const embi_js_code *code, const embi_js_dyn_site *s)
{
    dref r;
    memset(&r, 0, sizeof r);
    r.key = embi_js_key_from_str(rt, embi_js_as_string(code->consts[s->name]));
    return r;
}

static int with_find(embi_js_runtime *rt, embi_js_object *obj, dref *out)
{
    int r = embi_js_with_has_binding(rt, obj, out->key);
    if (r == EMBI_JS_YES) {
        out->obj = obj;
        out->rec = nullptr;
        out->box = nullptr;
    }
    return r;
}

static int rec_find(embi_js_runtime *rt, embi_js_object *rec, dref *out)
{
    embi_js_pdesc d;
    if (embi_js_obj_get_own_property(rt, rec, out->key, &d) != EMBI_JS_YES) {
        return EMBI_JS_NO;
    }
    out->obj = nullptr;
    out->rec = rec;
    out->box = (embi_js_box *)embi_js_as_cell(d.value);
    /* A name that is not writable in its record is read-only (an import of a module). */
    out->ro = (d.attrs & EMBI_JS_ATTR_WRITABLE) == 0;
    return EMBI_JS_YES;
}

/* Searches an eval context: pairs of (kind, payload), innermost first. */
static int ctx_find(embi_js_runtime *rt, const embi_js_varray *ctx, dref *out)
{
    for (uint32_t i = 0; i + 1 < ctx->hdr.aux; i += 2) {
        int kind = embi_js_as_int32(ctx->elems[i]);
        embi_js_value p = ctx->elems[i + 1];
        int r = EMBI_JS_NO;
        if (kind == EMBI_JS_DYN_WITH) {
            r = with_find(rt, embi_js_as_object(p), out);
        } else if (kind == EMBI_JS_DYN_REC || kind == EMBI_JS_DYN_FN_REC ||
                   kind == EMBI_JS_DYN_VE) {
            r = rec_find(rt, embi_js_as_object(p), out);
            if (r == EMBI_JS_YES) {
                out->deletable = kind == EMBI_JS_DYN_VE;
                out->ro = kind == EMBI_JS_DYN_VE ? false : out->ro;
            }
        } else if (kind == EMBI_JS_DYN_CHAIN) {
            r = ctx_find(rt, (const embi_js_varray *)embi_js_as_cell(p), out);
        }
        if (r != EMBI_JS_NO) {
            return r;
        }
    }
    return EMBI_JS_NO;
}

/* Looks a name up in a dynamic site's entries of the running frame, innermost first. */
static int dyn_lookup(embi_js_runtime *rt, const embi_js_code *code, const embi_js_value *slots,
                      const embi_js_value *upv, const embi_js_dyn_site *s, dref *out)
{
    for (uint32_t i = 0; i < s->n; i++) {
        const embi_js_dyn_ent *e = &code->dents[s->first + i];
        embi_js_value v = ent_value(slots, upv, e->src, e->index);
        int r;
        if (e->type == EMBI_JS_DYN_WITH) {
            r = with_find(rt, embi_js_as_object(v), out);
        } else if (e->type == EMBI_JS_DYN_CHAIN) {
            r = ctx_find(rt, (const embi_js_varray *)embi_js_as_cell(v), out);
        } else {
            r = rec_find(rt, embi_js_as_object(v), out);
            if (r == EMBI_JS_YES) {
                out->deletable = true;
                out->ro = e->type == EMBI_JS_DYN_VE ? false : out->ro;
            }
        }
        if (r != EMBI_JS_NO) {
            return r;
        }
    }
    return EMBI_JS_NO;
}

/* GetBindingValue of an object environment record: the property may have gone since the lookup,
 * which is checked again (a second [[HasProperty]]) and is a ReferenceError in strict code. */
static embi_js_value dref_get(embi_js_runtime *rt, const dref *r, bool strict)
{
    if (r->box) {
        return r->box->value;
    }
    int has = embi_js_obj_has_property(rt, r->obj, r->key);
    if (has == EMBI_JS_THREW) {
        return EXC;
    }
    if (has == EMBI_JS_NO) {
        if (strict) {
            return throw_name(rt, EMBI_JS_PROTO_REFERENCE_ERROR, "", embi_js_key_to_value(rt, r->key),
                              " is not defined");
        }
        return UNDEF;
    }
    return embi_js_obj_get(rt, r->obj, r->key, embi_js_from_object(r->obj));
}

/* The box of a function expression's self-name is immutable (hdr.aux is 1, set by build_context):
 * a sloppy store is ignored and a strict store is a TypeError. */
static bool box_is_ro(const embi_js_box *b)
{
    return b->hdr.aux != 0;
}

static int box_store_ro(embi_js_runtime *rt, bool strict)
{
    if (strict) {
        (void)embi_js_throw_type_error(rt, "Assignment to constant variable.");
        return EMBI_JS_THREW;
    }
    return EMBI_JS_YES;
}

static int dref_put(embi_js_runtime *rt, dref *r, embi_js_value v, bool strict)
{
    if (r->box) {
        if (r->ro || box_is_ro(r->box)) {
            return box_store_ro(rt, strict);
        }
        r->box->value = v;
        EMBI_JS_WB(r->box, v);
        return EMBI_JS_YES;
    }
    /* SetMutableBinding of an object environment record: [[HasProperty]] first (always), and a
     * property that has gone is a ReferenceError in strict code. */
    int has = embi_js_obj_has_property(rt, r->obj, r->key);
    if (has == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    if (has == EMBI_JS_NO && strict) {
        (void)throw_name(rt, EMBI_JS_PROTO_REFERENCE_ERROR, "", embi_js_key_to_value(rt, r->key),
                         " is not defined");
        return EMBI_JS_THREW;
    }
    return embi_js_put(rt, r->obj, r->key, v, strict);
}

/* The context of a direct eval call: for each scope entry of the site, a (kind, payload) pair.
 * A DECL group becomes a record of the boxes it names. */
static embi_js_value build_context(embi_js_runtime *rt, const embi_js_code *code,
                                   const embi_js_value *slots, const embi_js_value *upv,
                                   const embi_js_eval_site *es)
{
    embi_js_varray *ctx = embi_js_varray_new(rt, 2u * es->n);
    for (uint32_t j = 0; j < es->n; j++) {
        const embi_js_eval_ent *e = &code->eents[es->first + j];
        embi_js_value kind;
        embi_js_value payload;
        if (e->type == EMBI_JS_DYN_DECL_LEX) {
            embi_js_object *rec = embi_js_object_new(rt, EMBI_JS_NULL);
            for (uint32_t m = 0; m < e->nnames; m++) {
                const embi_js_eval_name *nm = &code->enames[e->first_name + m];
                embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(code->consts[nm->name]));
                embi_js_define_raw(rt, rec, k, EMBI_JS_TRUE, 0);
            }
            kind = embi_js_from_int32(EMBI_JS_DYN_LEXREC);
            payload = embi_js_from_object(rec);
        } else if (e->type == EMBI_JS_DYN_DECL || e->type == EMBI_JS_DYN_DECL_FN) {
            embi_js_object *rec = embi_js_object_new(rt, EMBI_JS_NULL);
            for (uint32_t m = 0; m < e->nnames; m++) {
                const embi_js_eval_name *nm = &code->enames[e->first_name + m];
                embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(code->consts[nm->name]));
                embi_js_value bv;
                uint32_t attrs = EMBI_JS_ATTR_WRITABLE;
                if (nm->src == EMBI_JS_SRC_IMPORT) {
                    /* An import: the exporter's own box, which is shared, so it is not marked
                     * read-only. The record's attributes make the name read-only (rec_find). */
                    bv = embi_jsm_import_box(code->consts[nm->tok]);
                    if (embi_js_is_empty(bv)) {
                        bv = embi_js_from_cell(embi_js_box_new(rt, EMBI_JS_EMPTY));
                    }
                    attrs = 0;
                } else {
                    embi_js_box *nb = ent_box(slots, upv, nm->src, nm->index);
                    if (nm->ro) {
                        nb->hdr.aux = 1;
                    }
                    bv = embi_js_from_cell(nb);
                }
                embi_js_define_raw(rt, rec, k, bv, attrs);
            }
            kind = embi_js_from_int32(e->type == EMBI_JS_DYN_DECL_FN ? EMBI_JS_DYN_FN_REC
                                                                     : EMBI_JS_DYN_REC);
            payload = embi_js_from_object(rec);
        } else {
            kind = embi_js_from_int32((int32_t)e->type);
            payload = ent_value(slots, upv, e->src, e->index);
        }
        EMBI_JS_STORE(ctx, ctx->elems[2u * j], kind);
        EMBI_JS_STORE(ctx, ctx->elems[2u * j + 1u], payload);
    }
    return embi_js_from_cell(ctx);
}

/* The record of the innermost variable environment in an eval context, or nullptr. */
static embi_js_object *ve_find(const embi_js_varray *ctx)
{
    for (uint32_t i = 0; i + 1 < ctx->hdr.aux; i += 2) {
        int kind = embi_js_as_int32(ctx->elems[i]);
        if (kind == EMBI_JS_DYN_VE) {
            return embi_js_as_object(ctx->elems[i + 1]);
        }
        if (kind == EMBI_JS_DYN_CHAIN) {
            embi_js_object *o = ve_find((const embi_js_varray *)embi_js_as_cell(ctx->elems[i + 1]));
            if (o) {
                return o;
            }
        }
    }
    return nullptr;
}

/* Finds a function-level box (a local, parameter, function, callee or arguments of the calling
 * function) before the first variable-environment record. Sets *hit_ve when the scan reached
 * the VE record, which ends the search. */
static embi_js_box *fn_box_find(embi_js_runtime *rt, const embi_js_varray *ctx, embi_js_key k,
                                bool *hit_ve)
{
    for (uint32_t i = 0; i + 1 < ctx->hdr.aux; i += 2) {
        int kind = embi_js_as_int32(ctx->elems[i]);
        if (kind == EMBI_JS_DYN_VE) {
            *hit_ve = true;
            return nullptr;
        }
        if (kind == EMBI_JS_DYN_FN_REC) {
            embi_js_pdesc d;
            if (embi_js_obj_get_own_property(rt, embi_js_as_object(ctx->elems[i + 1]), k, &d) ==
                EMBI_JS_YES) {
                return (embi_js_box *)embi_js_as_cell(d.value);
            }
        } else if (kind == EMBI_JS_DYN_CHAIN) {
            embi_js_box *b =
                fn_box_find(rt, (const embi_js_varray *)embi_js_as_cell(ctx->elems[i + 1]), k, hit_ve);
            if (b || *hit_ve) {
                return b;
            }
        }
    }
    return nullptr;
}

/* Declares an eval code's var or function name in the caller's variable environment (or on
 * the global object when the caller has none). has_value stores v under the name. */
/* Whether a let, const or block function between the eval call and its function's top scope has
 * this name (an eval var of it is a SyntaxError). *stop is set at the function's variable
 * environment, where the search ends. */
static bool ctx_lex_conflict(embi_js_runtime *rt, const embi_js_varray *ctx, embi_js_key k, bool *stop)
{
    for (uint32_t i = 0; i + 1 < ctx->hdr.aux; i += 2) {
        int kind = embi_js_as_int32(ctx->elems[i]);
        if (kind == EMBI_JS_DYN_VE) {
            *stop = true;
            return false;
        }
        if (kind == EMBI_JS_DYN_LEXREC) {
            if (embi_js_obj_has_own_property(rt, embi_js_as_object(ctx->elems[i + 1]), k) ==
                EMBI_JS_YES) {
                return true;
            }
        } else if (kind == EMBI_JS_DYN_CHAIN) {
            if (ctx_lex_conflict(rt, (const embi_js_varray *)embi_js_as_cell(ctx->elems[i + 1]), k,
                                 stop)) {
                return true;
            }
            if (*stop) {
                return false;
            }
        }
    }
    return false;
}

/* The checks of EvalDeclarationInstantiation for one name, run for all names before any is declared:
 * a lexical conflict is a SyntaxError, and a global object that cannot take the declaration is a
 * TypeError. kind is 1 for a var and 2 for a function. */
static int eval_check(embi_js_runtime *rt, const embi_js_value *upv, embi_js_key k, uint32_t kind)
{
    embi_js_value ctx_v = ((embi_js_box *)embi_js_as_cell(upv[0]))->value;
    const embi_js_varray *ctx = (const embi_js_varray *)embi_js_as_cell(ctx_v);
    bool stop = false;
    bool lc;
    if (ctx_lex_conflict(rt, ctx, k, &stop) || (!stop && lex_find(rt, k, &lc))) {
        (void)throw_name(rt, EMBI_JS_PROTO_SYNTAX_ERROR, "Identifier '", embi_js_key_to_value(rt, k),
                         "' has already been declared");
        return EMBI_JS_THREW;
    }
    if (ve_find(ctx)) {
        return EMBI_JS_YES;
    }
    embi_js_object *g = global_of(rt);
    embi_js_pdesc d;
    int own = embi_js_obj_get_own_property(rt, g, k, &d);
    if (own == EMBI_JS_THREW) {
        return EMBI_JS_THREW;
    }
    bool ok = true;
    if (own == EMBI_JS_NO) {
        ok = embi_js_obj_is_extensible(g);
    } else if (kind == 2) {
        ok = (d.attrs & EMBI_JS_ATTR_CONFIGURABLE) ||
             (!embi_js_pd_is_accessor(&d) && (d.attrs & EMBI_JS_ATTR_WRITABLE) &&
              (d.attrs & EMBI_JS_ATTR_ENUMERABLE));
    }
    if (!ok) {
        (void)throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot declare global name '",
                         embi_js_key_to_value(rt, k), "'");
        return EMBI_JS_THREW;
    }
    return EMBI_JS_YES;
}

static int eval_bind(embi_js_runtime *rt, const embi_js_value *upv, embi_js_key k, bool has_value,
                     embi_js_value v)
{
    embi_js_value ctx_v = ((embi_js_box *)embi_js_as_cell(upv[0]))->value;
    embi_js_object *ve = ve_find((const embi_js_varray *)embi_js_as_cell(ctx_v));
    embi_js_pdesc d;
    if (!ve) {
        embi_js_object *g = global_of(rt);
        int own = embi_js_obj_get_own_property(rt, g, k, &d);
        if (own == EMBI_JS_NO) {
            if (!embi_js_obj_is_extensible(g)) {
                (void)throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot declare global variable '",
                                 embi_js_key_to_value(rt, k), "'");
                return EMBI_JS_THREW;
            }
            embi_js_define_raw(rt, g, k, has_value ? v : UNDEF,
                               EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE |
                                   EMBI_JS_ATTR_CONFIGURABLE);
        } else if (has_value) {
            if (d.attrs & EMBI_JS_ATTR_CONFIGURABLE) {
                embi_js_define_raw(rt, g, k, v,
                                   EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE |
                                       EMBI_JS_ATTR_CONFIGURABLE);
            } else if (embi_js_pd_is_accessor(&d) || !(d.attrs & EMBI_JS_ATTR_WRITABLE) ||
                       !(d.attrs & EMBI_JS_ATTR_ENUMERABLE)) {
                (void)throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot declare global function '",
                                 embi_js_key_to_value(rt, k), "'");
                return EMBI_JS_THREW;
            } else if (embi_js_put(rt, g, k, v, false) == EMBI_JS_THREW) {
                return EMBI_JS_THREW;
            }
        }
        return EMBI_JS_YES;
    }
    embi_js_box *b;
    bool hit_ve = false;
    b = fn_box_find(rt, (const embi_js_varray *)embi_js_as_cell(ctx_v), k, &hit_ve);
    if (b) {
        if (has_value) {
            b->value = v;
            EMBI_JS_WB(b, v);
        }
        return EMBI_JS_YES;
    }
    if (embi_js_obj_get_own_property(rt, ve, k, &d) == EMBI_JS_YES) {
        b = (embi_js_box *)embi_js_as_cell(d.value);
    } else {
        b = embi_js_box_new(rt, UNDEF);
        embi_js_define_raw(rt, ve, k, embi_js_from_cell(b), EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_ENUMERABLE);
    }
    if (has_value) {
        b->value = v;
        EMBI_JS_WB(b, v);
    }
    return EMBI_JS_YES;
}

/* Whether the context of a direct eval (its scopes, innermost first) declares the name. */
static bool ctx_declares(embi_js_runtime *rt, const embi_js_varray *ctx, embi_js_key k)
{
    for (uint32_t i = 0; i + 1 < ctx->hdr.aux; i += 2) {
        int kind = embi_js_as_int32(ctx->elems[i]);
        if (kind == EMBI_JS_DYN_LEXREC || kind == EMBI_JS_DYN_REC || kind == EMBI_JS_DYN_FN_REC) {
            if (embi_js_obj_has_own_property(rt, embi_js_as_object(ctx->elems[i + 1]), k) ==
                EMBI_JS_YES) {
                return true;
            }
        } else if (kind == EMBI_JS_DYN_CHAIN &&
                   ctx_declares(rt, (const embi_js_varray *)embi_js_as_cell(ctx->elems[i + 1]), k)) {
            return true;
        }
    }
    return false;
}

typedef struct eval_priv_arg {
    embi_js_runtime *rt;
    embi_js_value ctx;
} eval_priv_arg;

/* priv_known callback of the eval compile. */
static bool eval_priv_known(void *user, embi_js_value name)
{
    const eval_priv_arg *a = user;
    embi_js_key k = embi_js_key_from_str(a->rt, embi_js_as_string(name));
    return ctx_declares(a->rt, (const embi_js_varray *)embi_js_as_cell(a->ctx), k);
}

/* Compiles and runs eval code with the given context. A non-string argument is the result. */
static embi_js_value eval_run(embi_js_runtime *rt, embi_js_realm *r, embi_js_value src,
                              embi_js_value ctx, bool strict, embi_js_value this_v,
                              embi_js_value new_target, unsigned eflags, embi_js_value home,
                              embi_js_value module)
{
    if (!embi_js_is_string(src)) {
        return src;
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_js_str_append_wtf8(embi_js_as_string(src), &sb);
    const char *text = sb.data ? sb.data : "";
    eval_priv_arg parg = {rt, ctx};
    embi_js_value code = embi_jsc_compile_eval(r, (const uint8_t *)text, sb.len, strict, eflags,
                                               eval_priv_known, &parg, module);
    embi_sb_free(&sb);
    if (code == EXC) {
        return EXC;
    }
    embi_js_object *f = make_closure(rt, code, nullptr, nullptr);
    embi_js_box *cb = embi_js_box_new(rt, ctx);
    EMBI_JS_STORE(f, closure_upvals(f)[0], embi_js_from_cell(cb));
    if (!embi_js_is_undefined(home)) {
        /* Eval code in a method sees the method's [[HomeObject]] (super.x). */
        cl_set_home(f, home);
    }
    return closure_call_nt(rt, f, this_v, 0, nullptr, new_target);
}

bool embi_js_restricted_get(embi_js_runtime *rt, embi_js_value getter, embi_js_value receiver,
                            embi_js_key key, embi_js_value *out)
{
    embi_js_vm *vm = rt->vm;
    if (!vm || !vm->realm || !vm->realm->thrower ||
        getter != embi_js_from_object(vm->realm->thrower)) {
        return false;
    }
    embi_js_closure *cl = closure_or_null(receiver);
    if (!cl) {
        return false;
    }
    const uint16_t barred = EMBI_JS_CODE_STRICT | EMBI_JS_CODE_NO_SELF | EMBI_JS_CODE_GENERATOR |
                            EMBI_JS_CODE_ASYNC | EMBI_JS_CODE_CLASS_CTOR;
    if (code_of(cl->code)->flags & barred) {
        return false;
    }
    bool is_caller = key == embi_js_key_c(rt, "caller");
    if (!is_caller && key != embi_js_key_c(rt, "arguments")) {
        return false;
    }
    *out = EMBI_JS_NULL;
    if (is_caller) {
        embi_js_object *f = embi_js_as_object(receiver);
        for (uint32_t i = vm->frames.len; i-- > 0;) {
            if (vm->frames.data[i].callee != f) {
                continue;
            }
            if (i > 0) {
                const embi_js_frame *up = &vm->frames.data[i - 1u];
                if (up->callee && closure_or_null(embi_js_from_object(up->callee)) &&
                    !(up->code->flags & EMBI_JS_CODE_STRICT)) {
                    *out = embi_js_from_object(up->callee);
                }
            }
            break;
        }
    }
    return true;
}

/* The realm's eval function, called indirectly: global scope, strict only by its own directive. */
static embi_js_value js_eval_native(embi_js_realm *r, embi_js_value this_v, int argc,
                                    const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)magic;
    embi_js_value src = argc > 0 ? argv[0] : UNDEF;
    embi_js_varray *ctx = embi_js_varray_new(r->rt, 0);
    return eval_run(r->rt, r, src, embi_js_from_cell(ctx), false, embi_js_from_object(r->global), UNDEF,
                    0, UNDEF, EMBI_JS_EMPTY);
}

/* ------------------------------------------------------------------ frames */

/* The native stack check: bytes used below the recorded base. Outside any embedder call the
 * current thread's stack top stands in for the base. */
static bool native_ok(embi_js_runtime *rt, const embi_js_vm *vm)
{
    uintptr_t base = rt->nest > 0 ? rt->stack_base : embi_js_stack_top();
    if (base == 0) {
        return true;
    }
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    return base <= here || base - here <= vm->native_limit;
}

/* Pushes a frame for code run with callee. Returns false (with the exception pending) when
 * the call depth is exceeded or a this-coercion throws. */
static bool push_frame(embi_js_runtime *rt, embi_js_object *callee, embi_js_code *code,
                       embi_js_value this_v, int argc, const embi_js_value *argv,
                       embi_js_value new_target, uint8_t flags)
{
    embi_js_vm *vm = rt->vm;
    if (vm->frames.len >= vm->max_depth) {
        (void)embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
        return false;
    }
    if (!(code->flags & EMBI_JS_CODE_STRICT) && !embi_js_is_object(this_v) &&
        !embi_js_is_empty(this_v)) {
        if (embi_js_is_nullish(this_v)) {
            this_v = embi_js_from_object(vm->realm->global);
        } else {
            embi_js_object *o = embi_js_to_object(rt, this_v);
            if (!o) {
                return false;
            }
            this_v = embi_js_from_object(o);
        }
    }
    uint32_t nslots = code->nparams + code->nlocals + code->max_stack;
    uint32_t prev = 0;
    uint32_t chunk = 0;
    embi_js_value *base = alloc_slots(vm, nslots, &chunk, &prev);
    for (uint32_t i = 0; i < code->nparams; i++) {
        base[i] = (int)i < argc ? argv[i] : UNDEF;
    }
    for (uint32_t i = code->nparams; i < nslots; i++) {
        base[i] = UNDEF;
    }
    if (vm->frames.len > 0) {
        /* An entry frame is pushed by a native or an accessor that an op of the frame below
         * called: remember that op for the stack trace. */
        vm->frames.data[vm->frames.len - 1u].cur = (flags & EMBI_JS_FRAME_ENTRY) ? vm->pc_cur : nullptr;
    }
    if ((code->flags & (EMBI_JS_CODE_ASYNC | EMBI_JS_CODE_GENERATOR)) == EMBI_JS_CODE_ASYNC) {
        flags |= EMBI_JS_FRAME_ASYNC;
    }
    /* CF2: the frame is written in its slot of the frame vector, field by field. A zeroed local copied in with
     * embi_vec_push cost a rep stos and a store-forwarding stall on every call (half of push_frame's time). Every
     * field is set here, as the memset of the local did. */
    if (vm->frames.len == vm->frames.cap) {
        embi_vec_reserve(&vm->frames, vm->frames.len + 1u);
    }
    embi_js_frame *f = &vm->frames.data[vm->frames.len];
    f->code = code;
    f->callee = callee;
    f->this_v = this_v;
    f->new_target = new_target;
    f->base = base;
    f->nslots = nslots;
    f->chunk = chunk;
    f->prev_chunk = prev;
    f->pc = code->bc;
    f->sp = base + code->nparams + code->nlocals;
    f->cur = nullptr;
    f->flags = flags;
    f->gen = nullptr;
    vm->frames.len++;
    embi_js_value *fb = base;
    if (code->flags & EMBI_JS_CODE_MAPPED_ARGS) {
        for (uint32_t i = 0; i < code->npmap; i++) {
            if (code->pmap[i]) {
                fb[i] = embi_js_from_cell(embi_js_box_new(rt, fb[i]));
            }
        }
    }
    if (code->arguments_slot != EMBI_JS_NO_SLOT) {
        embi_js_object *args = make_arguments(rt, callee, code, argc, argv, fb);
        fb[code->arguments_slot] = embi_js_from_object(args);
    }
    if (code->flags & EMBI_JS_CODE_REST) {
        uint32_t k = code->rest_slot;
        embi_js_object *ra = embi_js_array_create_default(rt, 0);
        bool ok = ra != nullptr;
        for (int i = (int)k; ok && i < argc; i++) {
            ok = embi_js_create_data_property(rt, ra, embi_js_key_from_index((uint32_t)(i - (int)k)),
                                              argv[i]) != EMBI_JS_THREW;
        }
        if (!ok) {
            pop_frame(vm);
            return false;
        }
        fb[k] = embi_js_from_object(ra);
    }
    return true;
}

static embi_js_value closure_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                  int argc, const embi_js_value *argv);
/* SUPER_BASE: the base of a super[key] reference, fixed before the key converts. */
static __attribute__((noinline)) embi_js_value super_base_new(embi_js_runtime *rt, embi_js_value home)
{
    embi_js_value proto = super_proto(rt, home);
    if (proto == EXC) {
        return EXC;
    }
    return embi_js_from_object(embi_js_object_new(rt, proto));
}

/* DYN_REF in strict code for a name that no scope declares: UNDEF, or null when the global
 * environment lacks the name now (an unresolvable reference), or EXC. */
static __attribute__((noinline)) embi_js_value dyn_ref_unres(embi_js_runtime *rt, embi_js_key key)
{
    bool lc;
    if (lex_find(rt, key, &lc)) {
        return UNDEF;
    }
    int has = embi_js_obj_has_property(rt, global_of(rt), key);
    if (has == EMBI_JS_THREW) {
        return EXC;
    }
    return has ? UNDEF : EMBI_JS_NULL;
}

/* The store through an unresolvable reference: a ReferenceError. */
static __attribute__((noinline)) embi_js_value dyn_set_unres_throw(embi_js_runtime *rt,
                                                                    const embi_js_code *code,
                                                                    const embi_js_dyn_site *s)
{
    dref r = dref_new(rt, code, s);
    return throw_name(rt, EMBI_JS_PROTO_REFERENCE_ERROR, "", embi_js_key_to_value(rt, r.key),
                      " is not defined");
}

static embi_js_value run(embi_js_runtime *rt);
static embi_js_value closure_call_nt(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                     int argc, const embi_js_value *argv, embi_js_value new_target);

/* An embedder entry starts with no frames. An interrupt is unwound by the outermost entry only,
 * so the stop state ends when that entry returns. */
static void outermost_begin(embi_js_vm *vm)
{
    if (vm->frames.len == 0) {
        vm->loc_valid = false;
    }
}

static void outermost_end(embi_js_vm *vm)
{
    if (vm->frames.len == 0) {
        vm->terminating = false;
    }
}

bool embi_js_native_stack_ok(embi_js_runtime *rt)
{
    if (!rt->vm) {
        return true;
    }
    return native_ok(rt, rt->vm);
}

static embi_js_value closure_call_nt(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                     int argc, const embi_js_value *argv, embi_js_value new_target)
{
    embi_js_vm *vm = rt->vm;
    if (!native_ok(rt, vm)) {
        return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    }
    embi_js_code *code = code_of(closure_of(f)->code);
    if (code->flags & EMBI_JS_CODE_CLASS_CTOR) {
        return embi_js_throw_type_error(rt, "Class constructor cannot be invoked without 'new'");
    }
    outermost_begin(vm);
    if (!push_frame(rt, f, code, this_v, argc, argv, new_target, EMBI_JS_FRAME_ENTRY)) {
        outermost_end(vm);
        return EXC;
    }
    const uint8_t *saved_pc = vm->pc_cur;
    embi_js_value r = run(rt);
    vm->pc_cur = saved_pc;
    outermost_end(vm);
    return r;
}

static embi_js_value closure_call(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                  int argc, const embi_js_value *argv)
{
    return closure_call_nt(rt, f, this_v, argc, argv, UNDEF);
}

static embi_js_value closure_construct(embi_js_runtime *rt, embi_js_object *f, int argc,
                                       const embi_js_value *argv, embi_js_value new_target)
{
    embi_js_vm *vm = rt->vm;
    if (!native_ok(rt, vm)) {
        return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    }
    embi_js_code *code = code_of(closure_of(f)->code);
    if (!(code->flags & EMBI_JS_CODE_CONSTRUCTOR)) {
        return embi_js_throw_type_error(rt, "not a constructor");
    }
    /* A derived constructor's this is uninitialized until super() returns; a base constructor
     * creates its object and runs the field initializers before its body. */
    embi_js_value self = EMBI_JS_EMPTY;
    if (!(code->flags & EMBI_JS_CODE_DERIVED)) {
        embi_js_value proto = embi_js_proto_from_ctor(rt, new_target, EMBI_JS_PROTO_OBJECT);
        if (proto == EXC) {
            return EXC;
        }
        self = embi_js_from_object(embi_js_object_new(rt, proto));
        if (!class_fields_init(rt, embi_js_from_object(f), self)) {
            return EXC;
        }
    }
    outermost_begin(vm);
    if (!push_frame(rt, f, code, self, argc, argv, new_target,
                    EMBI_JS_FRAME_ENTRY | EMBI_JS_FRAME_CONSTRUCT)) {
        outermost_end(vm);
        return EXC;
    }
    const uint8_t *saved_pc = vm->pc_cur;
    embi_js_value r = run(rt);
    vm->pc_cur = saved_pc;
    outermost_end(vm);
    return r;
}

/* ------------------------------------------------------------------ generators (J2c)
 *
 * Design. A generator body runs in an ordinary frame of the interpreter loop, so a yield returns
 * from run() and does not recurse in C or switch stacks. While a generator is suspended, its frame
 * lives in the generator object (embi_js_gen): the callee, the this value, the live slots
 * [0, sp - base) (parameters, locals and the operand stack, so a yield inside an expression keeps
 * its partial operands), and the byte offset to resume at. Resuming pushes a frame on the VM stack
 * (alloc_slots), copies the saved slots back and pushes [received, mode] for YIELD. Yielding or
 * completing pops the frame again. saved holds only the live slots: GEN_START sizes it to the
 * parameters and locals, and a yield that needs more grows it (gen_reserve), up to the frame size.
 * A generator that never yields past its start keeps the small buffer.
 *
 * Creation. GEN_START sits at the body start, after the parameters are bound, so default values
 * and destructuring run at call time as the spec requires. GEN_START creates the generator object
 * (its prototype is the callee's prototype property when that is an object), saves the frame, and
 * returns the object to the caller in place of a body result.
 *
 * Resumption is always entered from a native (%GeneratorPrototype% next, throw and return) with
 * the ENTRY flag, so run() returns when the body yields or completes. embi_js_generator_resume
 * tells the two apart by the state the body left.
 *
 * Reuse by J3. An async function is a generator whose awaits are yields to the job queue: the
 * frame, the save and the restore are these same functions, and only the resumer (a job instead of
 * next()) and the result handling differ.
 */

static embi_js_gen *gen_of(const embi_js_object *o)
{
    return o->internal;
}

static void gen_trace(embi_js_object *o, embi_js_tracer *t)
{
    embi_js_gen *g = gen_of(o);
    if (!g) {
        return;
    }
    embi_js_mark_value(t, g->callee);
    embi_js_mark_value(t, g->this_v);
    for (uint32_t i = 0; i < g->nsaved; i++) {
        embi_js_mark_value(t, g->saved[i]);
    }
    embi_js_mark_value(t, g->promise);
    embi_js_mark_value(t, g->qhead);
    embi_js_mark_value(t, g->qtail);
    t->external += sizeof *g + arr_bytes(g->cap, sizeof(embi_js_value));
}

/* Drops the saved frame of a generator that can no longer resume. */
static void gen_release(embi_js_gen *g)
{
    if (g->saved) {
        embi_free(g->saved, arr_bytes(g->cap, sizeof(embi_js_value)));
        g->saved = nullptr;
    }
    g->cap = 0;
    g->nsaved = 0;
}

static void gen_finalize(embi_js_object *o)
{
    embi_js_gen *g = gen_of(o);
    if (!g) {
        return;
    }
    gen_release(g);
    embi_free(g, sizeof *g);
    o->internal = nullptr;
}

const embi_js_class embi_js_class_generator = {
    .name = "Generator",
    .flags = EMBI_JS_CLASS_IC,
    .trace = gen_trace,
    .finalize = gen_finalize,
};

bool embi_js_is_generator(embi_js_value v)
{
    return embi_js_is_object(v) && embi_js_as_object(v)->klass == &embi_js_class_generator;
}

embi_js_value embi_js_iter_result_new(embi_js_runtime *rt, embi_js_value v, bool done)
{
    embi_js_object *o = embi_js_object_new_plain(rt);
    embi_js_define_raw(rt, o, embi_js_name_val(rt, EMBI_JS_NAME_VALUE), v, EMBI_JS_ATTR_DEFAULT);
    embi_js_define_raw(rt, o, embi_js_iter_key(rt, EMBI_JS_IK_DONE), embi_js_from_bool(done),
                       EMBI_JS_ATTR_DEFAULT);
    return embi_js_from_object(o);
}

/* Grows the saved buffer to hold need slots. The growth is at least a half again, capped at the
 * frame size, which is the most any frame saves. */
static void gen_reserve(embi_js_gen *g, uint32_t need, const embi_js_code *code)
{
    if (need <= g->cap) {
        return;
    }
    uint32_t limit = code->nparams + code->nlocals + code->max_stack;
    uint32_t ncap = g->cap + g->cap / 2u;
    if (ncap < need) {
        ncap = need;
    }
    if (ncap > limit) {
        ncap = limit;
    }
    g->saved = embi_realloc(g->saved, arr_bytes(g->cap, sizeof(embi_js_value)),
                            arr_bytes(ncap, sizeof(embi_js_value)));
    g->cap = ncap;
}

/* Saves the live slots [slots, live_end) of the running generator frame, the resume offset and
 * why the frame is suspended. */
static void gen_save(embi_js_vm *vm, const embi_js_value *slots, const embi_js_value *live_end,
                     const uint8_t *resume, uint8_t susp)
{
    embi_js_frame *f = &vm->frames.data[vm->frames.len - 1u];
    embi_js_gen *g = gen_of(f->gen);
    uint32_t live = (uint32_t)(live_end - slots);
    gen_reserve(g, live, f->code);
    /* The owner is the generator object: it may be old (a long-lived generator that survived a
     * minor), and the saved slots are traced only through it. */
    for (uint32_t i = 0; i < live; i++) {
        EMBI_JS_STORE(f->gen, g->saved[i], slots[i]);
    }
    g->nsaved = live;
    g->pc = (uint32_t)(resume - f->code->bc);
    g->susp = susp;
}

/* A yield: saves the frame and marks the generator suspended at it. */
static void gen_suspend(embi_js_vm *vm, const embi_js_value *slots, const embi_js_value *live_end,
                        const uint8_t *resume, bool raw)
{
    embi_js_frame *f = &vm->frames.data[vm->frames.len - 1u];
    embi_js_gen *g = gen_of(f->gen);
    gen_save(vm, slots, live_end, resume, EMBI_JS_SUSP_YIELD);
    g->raw = raw;
    g->state = EMBI_JS_GEN_SUSPENDED_YIELD;
}

/* A new generator state with room for live slots. */
static embi_js_gen *gen_new(uint32_t live)
{
    embi_js_gen *g = embi_calloc(1, sizeof *g);
    g->cap = live ? live : 1u;
    g->saved = embi_calloc(g->cap, sizeof(embi_js_value));
    g->promise = EMBI_JS_UNDEFINED;
    g->qhead = EMBI_JS_EMPTY;
    g->qtail = EMBI_JS_EMPTY;
    return g;
}

/* Runs the body of the generator, async function or async generator o from its saved frame until
 * it yields, awaits, returns or throws. A body suspended at a yield or an await gets [v, mode]
 * pushed (mode 0 next or fulfilled, 1 throw or rejected, 2 return). Returns the body's result or
 * EMBI_JS_EXCEPTION. The caller reads g->susp to tell a suspension from a completion, and has
 * checked the limits. */
static embi_js_value gen_enter(embi_js_runtime *rt, embi_js_object *o, int mode, embi_js_value v)
{
    embi_js_gen *g = gen_of(o);
    embi_js_vm *vm = rt->vm;
    embi_js_code *code = code_of(closure_of(embi_js_as_object(g->callee))->code);
    uint32_t nslots = code->nparams + code->nlocals + code->max_stack;
    uint32_t prev = 0;
    uint32_t chunk = 0;
    bool pushed = g->susp != EMBI_JS_SUSP_NONE;
    uint32_t live = g->nsaved;
    bool async_fn =
        (code->flags & (EMBI_JS_CODE_ASYNC | EMBI_JS_CODE_GENERATOR)) == EMBI_JS_CODE_ASYNC;
    outermost_begin(vm);
    embi_js_value *base = alloc_slots(vm, nslots, &chunk, &prev);
    for (uint32_t i = 0; i < live; i++) {
        base[i] = g->saved[i];
    }
    for (uint32_t i = live; i < nslots; i++) {
        base[i] = UNDEF;
    }
    g->nsaved = 0;
    embi_js_value *sp0 = base + live;
    if (pushed) {
        sp0[0] = v;
        sp0[1] = embi_js_from_int32(mode);
        sp0 += 2;
    }
    g->susp = EMBI_JS_SUSP_NONE;
    if (vm->frames.len > 0) {
        vm->frames.data[vm->frames.len - 1u].cur = vm->pc_cur;
    }
    embi_js_frame f;
    memset(&f, 0, sizeof f);
    f.code = code;
    f.callee = embi_js_as_object(g->callee);
    f.this_v = g->this_v;
    f.new_target = UNDEF;
    f.base = base;
    f.nslots = nslots;
    f.chunk = chunk;
    f.prev_chunk = prev;
    f.pc = code->bc + g->pc;
    f.sp = sp0;
    f.flags = EMBI_JS_FRAME_ENTRY | (async_fn ? EMBI_JS_FRAME_ASYNC : EMBI_JS_FRAME_GEN);
    f.gen = o;
    embi_vec_push(&vm->frames, f);
    g->state = EMBI_JS_GEN_EXECUTING;
    const uint8_t *saved_pc = vm->pc_cur;
    embi_js_value r = run(rt);
    vm->pc_cur = saved_pc;
    outermost_end(vm);
    return r;
}

/* Resumes a suspended generator. mode 0 is next, 1 throw and 2 return. Returns the iterator result
 * object (or the inner result object for a yield*), or EMBI_JS_EXCEPTION with the exception
 * pending. */
embi_js_value embi_js_generator_resume(embi_js_runtime *rt, embi_js_object *o, int mode,
                                       embi_js_value v)
{
    embi_js_gen *g = gen_of(o);
    if (g->state == EMBI_JS_GEN_EXECUTING) {
        return embi_js_throw_type_error(rt, "Generator is already running");
    }
    if (g->state == EMBI_JS_GEN_SUSPENDED_START && mode != 0) {
        g->state = EMBI_JS_GEN_COMPLETED;
        gen_release(g);
    }
    if (g->state == EMBI_JS_GEN_COMPLETED) {
        if (mode == 1) {
            return embi_js_throw_value(rt, v);
        }
        return embi_js_iter_result_new(rt, mode == 2 ? v : UNDEF, true);
    }
    embi_js_vm *vm = rt->vm;
    if (!native_ok(rt, vm)) {
        return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    }
    if (vm->frames.len >= vm->max_depth) {
        return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    }
    embi_js_value r = gen_enter(rt, o, mode, v);
    if (r == EXC) {
        g->state = EMBI_JS_GEN_COMPLETED;
        gen_release(g);
        return EXC;
    }
    if (g->state == EMBI_JS_GEN_EXECUTING) {
        g->state = EMBI_JS_GEN_COMPLETED;
        gen_release(g);
        return embi_js_iter_result_new(rt, r, true);
    }
    if (g->raw) {
        return r;
    }
    return embi_js_iter_result_new(rt, r, false);
}

/* ------------------------------------------------------------------ async functions and async
 * generators (J3b)
 *
 * An async function's frame keeps a holder object (class embi_js_class_async) in frame->gen. The
 * holder's gen state holds the promise, which ASYNC_START creates; from then on the frame runs in
 * place. AWAIT saves the frame and subscribes two await closures (class class_await_fn) to the
 * awaited promise. A closure resumes the frame with [value, mode] through gen_enter. A RETURN, or
 * a throw that no handler catches, settles the promise, and the frame returns the promise (to its
 * caller, or to the job that resumed it). A throw before ASYNC_START (parameter binding) rejects a
 * new promise instead.
 *
 * An async generator object (class embi_js_class_async_gen) is a generator object whose gen state
 * also keeps the request queue. next, throw and return enqueue a request (a record [value, mode,
 * promise, next]). A request resumes the body when it is suspended at a yield. The body's yield or
 * completion settles the queue head (agen_complete), and the queue drain and the awaits of return
 * requests follow ECMA-262 27.6.3. */

const embi_js_class embi_js_class_async = {
    .name = "AsyncFunction",
    .flags = EMBI_JS_CLASS_IC,
    .trace = gen_trace,
    .finalize = gen_finalize,
};

const embi_js_class embi_js_class_async_gen = {
    .name = "AsyncGenerator",
    .flags = EMBI_JS_CLASS_IC,
    .trace = gen_trace,
    .finalize = gen_finalize,
};

/* The kinds of the await closures (their state is [holder, kind]). */
enum await_kind {
    AK_FN_OK,     /* a fulfilled await of an async function or async generator body */
    AK_FN_ERR,    /* a rejected await of the same */
    AK_AGRET_OK,  /* AsyncGeneratorAwaitReturn: the awaited value of a return request fulfilled */
    AK_AGRET_ERR, /* the same, rejected */
};

static embi_js_varray *vr_of(embi_js_value v)
{
    return (embi_js_varray *)embi_js_as_cell(v);
}

static embi_js_value agen_run(embi_js_runtime *rt, embi_js_object *hold, int mode, embi_js_value v);
static void agen_drain(embi_js_runtime *rt, embi_js_object *hold);
static embi_js_value agen_return_settled(embi_js_runtime *rt, embi_js_object *hold, bool rejected,
                                         embi_js_value v);

/* Resumes an async function body after an await (mode 0 fulfilled, 1 rejected). The body settles
 * its own promise, so the result is undefined unless the interrupt unwinds it. */
static embi_js_value async_resume(embi_js_runtime *rt, embi_js_object *hold, int mode,
                                  embi_js_value v)
{
    embi_js_gen *g = gen_of(hold);
    embi_js_vm *vm = rt->vm;
    if (!native_ok(rt, vm) || vm->frames.len >= vm->max_depth) {
        return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    }
    embi_js_value r = gen_enter(rt, hold, mode, v);
    if (r == EXC) {
        return EXC;
    }
    if (g->susp == EMBI_JS_SUSP_NONE) {
        g->state = EMBI_JS_GEN_COMPLETED;
        gen_release(g);
    }
    return UNDEF;
}

/* The await closures. Each kind has its own class (its call entry is the kind, as a native
 * function's magic is), and the closure's internal is the holder itself, so an await allocates two
 * closures and no state record. A holder is an object, so the trace marks it as a cell. */
static embi_js_value await_call(embi_js_runtime *rt, embi_js_object *f, int kind, int argc,
                                const embi_js_value *argv)
{
    embi_js_object *hold = (embi_js_object *)f->internal;
    embi_js_value v = argc > 0 ? argv[0] : UNDEF;
    if (kind == AK_AGRET_OK || kind == AK_AGRET_ERR) {
        return agen_return_settled(rt, hold, kind == AK_AGRET_ERR, v);
    }
    int mode = kind == AK_FN_ERR ? 1 : 0;
    if (hold->klass == &embi_js_class_async_gen) {
        return agen_run(rt, hold, mode, v);
    }
    return async_resume(rt, hold, mode, v);
}

static embi_js_value await_fn_ok(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)this_v;
    return await_call(rt, f, AK_FN_OK, argc, argv);
}

static embi_js_value await_fn_err(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                  int argc, const embi_js_value *argv)
{
    (void)this_v;
    return await_call(rt, f, AK_FN_ERR, argc, argv);
}

static embi_js_value agret_fn_ok(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                 int argc, const embi_js_value *argv)
{
    (void)this_v;
    return await_call(rt, f, AK_AGRET_OK, argc, argv);
}

static embi_js_value agret_fn_err(embi_js_runtime *rt, embi_js_object *f, embi_js_value this_v,
                                  int argc, const embi_js_value *argv)
{
    (void)this_v;
    return await_call(rt, f, AK_AGRET_ERR, argc, argv);
}

static void await_fn_trace(embi_js_object *o, embi_js_tracer *t)
{
    if (o->internal != nullptr) {
        embi_js_mark_cell(t, (embi_js_cell *)o->internal);
    }
}

static const embi_js_class class_await_ok = {
    .name = "Function", .call = await_fn_ok, .trace = await_fn_trace,
};
static const embi_js_class class_await_err = {
    .name = "Function", .call = await_fn_err, .trace = await_fn_trace,
};
static const embi_js_class class_agret_ok = {
    .name = "Function", .call = agret_fn_ok, .trace = await_fn_trace,
};
static const embi_js_class class_agret_err = {
    .name = "Function", .call = agret_fn_err, .trace = await_fn_trace,
};

/* A reaction closure for an await of hold (an async frame or an async generator). */
static embi_js_value await_fn_new(embi_js_runtime *rt, embi_js_object *hold, int kind)
{
    static const embi_js_class *const classes[] = {
        &class_await_ok, &class_await_err, &class_agret_ok, &class_agret_err};
    embi_js_object *f = embi_js_object_new_class(rt, EMBI_JS_NULL, classes[kind]);
    f->internal = hold;
    EMBI_JS_WB(f, hold);
    return embi_js_from_object(f);
}

/* Settles the promise of the async function whose holder is hold (nullptr before ASYNC_START)
 * with v, and returns the promise. */
static embi_js_value async_settle(embi_js_runtime *rt, embi_js_object *hold, embi_js_value v,
                                  bool rejected)
{
    embi_js_object *p = nullptr;
    if (hold && hold->klass == &embi_js_class_async) {
        embi_js_value pv = gen_of(hold)->promise;
        if (embi_js_is_object(pv)) {
            p = embi_js_as_object(pv);
        }
    }
    if (!p) {
        p = embi_js_promise_new(rt);
        if (hold && hold->klass == &embi_js_class_async) {
            gen_of(hold)->promise = embi_js_from_object(p);
            EMBI_JS_WB(hold, gen_of(hold)->promise);
        }
    }
    if (rejected) {
        (void)embi_js_promise_reject_with(rt, p, v);
    } else {
        (void)embi_js_promise_resolve_with(rt, p, v);
    }
    return embi_js_from_object(p);
}

/* ------------------------------------------------------------------ async generators (J3b) */

enum ag_mode { AG_NEXT = 0, AG_THROW = 1, AG_RETURN = 2 };

/* AsyncGeneratorEnqueue: appends the request [value, mode, promise, next] to the queue. */
static void agen_enqueue(embi_js_runtime *rt, embi_js_object *hold, embi_js_value v, int mode,
                         embi_js_value promise)
{
    embi_js_gen *g = gen_of(hold);
    embi_js_varray *rq = embi_js_varray_new(rt, 4);
    EMBI_JS_STORE(rq, rq->elems[0], v);
    EMBI_JS_STORE(rq, rq->elems[1], embi_js_from_int32(mode));
    EMBI_JS_STORE(rq, rq->elems[2], promise);
    EMBI_JS_STORE(rq, rq->elems[3], EMBI_JS_EMPTY);
    embi_js_value rv = embi_js_from_cell(rq);
    if (g->qtail == EMBI_JS_EMPTY) {
        g->qhead = rv;
        EMBI_JS_WB(hold, rv);
    } else {
        embi_js_varray *tailq = vr_of(g->qtail);
        EMBI_JS_STORE(tailq, tailq->elems[3], rv);
    }
    g->qtail = rv;
    EMBI_JS_WB(hold, rv);
}

/* AsyncGeneratorCompleteStep: removes the queue head and settles its promise, with a throw
 * completion (is_throw) or with the iterator result {value, done}. */
static void agen_complete(embi_js_runtime *rt, embi_js_object *hold, bool is_throw, embi_js_value v,
                          bool done)
{
    embi_js_gen *g = gen_of(hold);
    if (g->qhead == EMBI_JS_EMPTY) {
        return;
    }
    embi_js_varray *rq = vr_of(g->qhead);
    embi_js_value promise = rq->elems[2];
    g->qhead = rq->elems[3];
    EMBI_JS_WB(hold, g->qhead);
    if (g->qhead == EMBI_JS_EMPTY) {
        g->qtail = EMBI_JS_EMPTY;
        EMBI_JS_WB(hold, g->qtail);
    }
    if (is_throw) {
        (void)embi_js_promise_reject_with(rt, embi_js_as_object(promise), v);
    } else {
        embi_js_value res = embi_js_iter_result_new(rt, v, done);
        (void)embi_js_promise_resolve_with(rt, embi_js_as_object(promise), res);
    }
}

/* AsyncGeneratorAwaitReturn: the head request is a return request; its value is awaited, and the
 * generator completes with the outcome (agen_return_settled). */
static void agen_await_return(embi_js_runtime *rt, embi_js_object *hold)
{
    embi_js_gen *g = gen_of(hold);
    embi_js_value v = vr_of(g->qhead)->elems[0];
    embi_js_value pv = embi_js_promise_resolve(rt, intrinsic(rt, EMBI_JS_PROTO_PROMISE_CTOR), v);
    if (pv == EXC) {
        embi_js_value ex = embi_js_exception_get(rt);
        embi_js_exception_clear(rt);
        g->state = EMBI_JS_GEN_COMPLETED;
        gen_release(g);
        agen_complete(rt, hold, true, ex, true);
        agen_drain(rt, hold);
        return;
    }
    embi_js_value ok = await_fn_new(rt, hold, AK_AGRET_OK);
    embi_js_value er = await_fn_new(rt, hold, AK_AGRET_ERR);
    (void)embi_js_promise_then(rt, embi_js_as_object(pv), ok, er, nullptr);
}

/* AsyncGeneratorDrainQueue: completes the queued requests that need no body (the body completed,
 * or the generator was not started). A return request awaits its value and stops the drain. */
static void agen_drain(embi_js_runtime *rt, embi_js_object *hold)
{
    embi_js_gen *g = gen_of(hold);
    while (g->qhead != EMBI_JS_EMPTY) {
        embi_js_varray *rq = vr_of(g->qhead);
        int mode = embi_js_as_int32(rq->elems[1]);
        if (mode == AG_RETURN) {
            g->state = EMBI_JS_GEN_AWAIT_RETURN;
            agen_await_return(rt, hold);
            return;
        }
        if (mode == AG_THROW) {
            agen_complete(rt, hold, true, rq->elems[0], true);
        } else {
            agen_complete(rt, hold, false, UNDEF, true);
        }
    }
}

/* The awaited value of a return request settled: the generator completes. */
static embi_js_value agen_return_settled(embi_js_runtime *rt, embi_js_object *hold, bool rejected,
                                         embi_js_value v)
{
    embi_js_gen *g = gen_of(hold);
    g->state = EMBI_JS_GEN_COMPLETED;
    gen_release(g);
    agen_complete(rt, hold, rejected, v, true);
    agen_drain(rt, hold);
    return UNDEF;
}

/* Runs an async generator body with the completion (mode, v) until it yields, awaits or completes.
 * A completion settles the queue head and drains the queue. */
static embi_js_value agen_run(embi_js_runtime *rt, embi_js_object *hold, int mode, embi_js_value v)
{
    embi_js_gen *g = gen_of(hold);
    embi_js_vm *vm = rt->vm;
    embi_js_value r;
    if (!native_ok(rt, vm) || vm->frames.len >= vm->max_depth) {
        r = embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    } else {
        r = gen_enter(rt, hold, mode, v);
    }
    if (r != EXC && g->susp != EMBI_JS_SUSP_NONE) {
        return UNDEF;
    }
    if (r == EXC && vm->terminating) {
        return EXC;
    }
    bool threw = r == EXC;
    embi_js_value out = r;
    if (threw) {
        out = embi_js_exception_get(rt);
        embi_js_exception_clear(rt);
    }
    g->state = EMBI_JS_GEN_COMPLETED;
    gen_release(g);
    agen_complete(rt, hold, threw, out, true);
    agen_drain(rt, hold);
    return UNDEF;
}

/* AsyncGenerator.prototype.next, throw and return (modes AG_NEXT, AG_THROW, AG_RETURN): returns the
 * promise of the request. self must be an async generator, otherwise the promise is rejected with
 * a TypeError. */
embi_js_value embi_js_async_gen_request(embi_js_runtime *rt, embi_js_value self, int mode,
                                        embi_js_value v)
{
    embi_js_object *promise = embi_js_promise_new(rt);
    embi_js_value pv = embi_js_from_object(promise);
    if (!embi_js_is_object(self) || embi_js_as_object(self)->klass != &embi_js_class_async_gen) {
        (void)embi_js_throw_type_error(rt, "not an AsyncGenerator");
        embi_js_value ex = embi_js_exception_get(rt);
        embi_js_exception_clear(rt);
        (void)embi_js_promise_reject_with(rt, promise, ex);
        return pv;
    }
    embi_js_object *hold = embi_js_as_object(self);
    embi_js_gen *g = gen_of(hold);
    if (mode == AG_NEXT) {
        if (g->state == EMBI_JS_GEN_COMPLETED) {
            embi_js_value res = embi_js_iter_result_new(rt, UNDEF, true);
            (void)embi_js_promise_resolve_with(rt, promise, res);
            return pv;
        }
        agen_enqueue(rt, hold, v, AG_NEXT, pv);
        if (g->state == EMBI_JS_GEN_SUSPENDED_START || g->state == EMBI_JS_GEN_SUSPENDED_YIELD) {
            if (agen_run(rt, hold, AG_NEXT, v) == EXC) {
                return EXC;
            }
        }
        return pv;
    }
    if (mode == AG_THROW) {
        if (g->state == EMBI_JS_GEN_SUSPENDED_START) {
            g->state = EMBI_JS_GEN_COMPLETED;
            gen_release(g);
        }
        if (g->state == EMBI_JS_GEN_COMPLETED) {
            (void)embi_js_promise_reject_with(rt, promise, v);
            return pv;
        }
        agen_enqueue(rt, hold, v, AG_THROW, pv);
        if (g->state == EMBI_JS_GEN_SUSPENDED_YIELD) {
            if (agen_run(rt, hold, AG_THROW, v) == EXC) {
                return EXC;
            }
        }
        return pv;
    }
    agen_enqueue(rt, hold, v, AG_RETURN, pv);
    if (g->state == EMBI_JS_GEN_SUSPENDED_START || g->state == EMBI_JS_GEN_COMPLETED) {
        g->state = EMBI_JS_GEN_AWAIT_RETURN;
        agen_await_return(rt, hold);
    } else if (g->state == EMBI_JS_GEN_SUSPENDED_YIELD) {
        if (agen_run(rt, hold, AG_RETURN, v) == EXC) {
            return EXC;
        }
    }
    return pv;
}

/* GetIterator(v, async) for for-await: the @@asyncIterator method, or the sync iterator wrapped by
 * CreateAsyncFromSyncIterator. Returns the record or EMBI_JS_EXCEPTION. */
static embi_js_value async_iter_init(embi_js_runtime *rt, embi_js_value v)
{
    embi_js_value method = embi_js_get_method(rt, v, embi_js_wk_sym(rt, EMBI_JS_WK_ASYNC_ITERATOR));
    if (method == EXC) {
        return EXC;
    }
    if (method == UNDEF) {
        embi_js_value sync = iter_init(rt, v, false);
        if (sync == EXC) {
            return EXC;
        }
        embi_js_value w = embi_js_async_from_sync_create(rt, sync);
        embi_js_value nx = embi_js_get(rt, embi_js_as_object(w), iter_keys(rt)->k_next);
        if (nx == EXC) {
            return EXC;
        }
        return ir_new(rt, w, nx, 0);
    }
    embi_js_value it = embi_js_call(rt, method, v, 0, nullptr);
    if (it == EXC) {
        return EXC;
    }
    if (!embi_js_is_object(it)) {
        return embi_js_throw_type_error(rt, "Result of the Symbol.asyncIterator method is not an object");
    }
    embi_js_value nx = embi_js_get(rt, embi_js_as_object(it), iter_keys(rt)->k_next);
    if (nx == EXC) {
        return EXC;
    }
    return ir_new(rt, it, nx, 0);
}

/* The iterator protocol for the builtins (the record format is above iter_init). */
embi_js_value embi_js_ir_open(embi_js_runtime *rt, embi_js_value v)
{
    return iter_init(rt, v, true);
}

int embi_js_ir_next(embi_js_runtime *rt, embi_js_value rec, embi_js_value *out)
{
    return ir_step(rt, ir_of(rec), out);
}

bool embi_js_ir_close(embi_js_runtime *rt, embi_js_value rec)
{
    return ir_close(rt, ir_of(rec));
}

void embi_js_ir_close_throw(embi_js_runtime *rt, embi_js_value rec)
{
    ir_close_throw(rt, ir_of(rec));
}

/* ------------------------------------------------------------------ superinstruction helpers (J5i) */

/* ++ and -- (delta 1 or -1) on a value the int32 and number fast paths don't take: ToNumeric,
 * then the BigInt or number step. Returns EXC on a throw. */
static embi_js_value op_step_slow(embi_js_runtime *rt, embi_js_value v, int32_t delta)
{
    embi_js_value n = embi_js_to_numeric(rt, v);
    if (n == EXC) {
        return EXC;
    }
    if (embi_js_is_bigint(n)) {
        return embi_js_bigint_inc(rt, n, delta);
    }
    return embi_js_number(embi_js_as_double(n) + delta);
}

/* ------------------------------------------------------------------ interrupts */

static bool interrupted(embi_js_runtime *rt)
{
    embi_js_vm *vm = rt->vm;
    return vm->interrupt && vm->interrupt(rt, vm->interrupt_user);
}

/* IsStrictlyEqual, as embi_js_strict_equal, with the common cases inline: numbers, the same value, and two atoms
 * (an atom is the only string with its contents, so two different atoms differ). */
static inline bool seq_values(embi_js_value a, embi_js_value b)
{
    if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
        return a == b;
    }
    if (embi_js_is_number(a) && embi_js_is_number(b)) {
        return embi_js_as_double(a) == embi_js_as_double(b);
    }
    if (embi_js_is_string(a) && embi_js_is_string(b)) {
        const embi_js_string *x = embi_js_as_string(a);
        const embi_js_string *y = embi_js_as_string(b);
        if (x == y) {
            return true;
        }
        if (embi_js_str_is_atom(x) && embi_js_str_is_atom(y)) {
            return false;
        }
        return embi_js_str_equal(x, y);
    }
    return embi_js_strict_equal(a, b);
}

/* ------------------------------------------------------------------ the loop */
#ifdef EMBI_JS_OPSTATS
/* Dynamic op statistics (J5b): single ops, adjacent pairs and triples within a basic block.
 * Blocks end after a jump, and at any step that does not fall through to the next op (calls
 * returning, throws, unwinding), so a pair or triple never crosses one. A jump target that
 * follows a fall-through op is not detected: the counts are dynamic, not exact basic blocks.
 * The counters are process-wide, so a multi-runtime measurement is not separable. */
#define OPST_NONE 256u
#define OPST_TRI_SLOTS (1u << 16)
static uint64_t opst_single[256];
static uint64_t opst_pair[256u * 256u];
static uint64_t opst_tri_key[OPST_TRI_SLOTS];
static uint64_t opst_tri_cnt[OPST_TRI_SLOTS];
static bool opst_end_tab[256];
static bool opst_end_init;
static const uint8_t *opst_expect;
static uint32_t opst_h1 = OPST_NONE;
static uint32_t opst_h2 = OPST_NONE;

static void opst_tri_add(uint32_t key)
{
    uint64_t k = (uint64_t)key + 1u; /* 0 marks an empty slot */
    uint32_t i = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 48);
    for (uint32_t n = 0; n < OPST_TRI_SLOTS; n++) {
        if (opst_tri_key[i] == k) {
            opst_tri_cnt[i]++;
            return;
        }
        if (opst_tri_key[i] == 0) {
            opst_tri_key[i] = k;
            opst_tri_cnt[i] = 1;
            return;
        }
        i = (i + 1u) & (OPST_TRI_SLOTS - 1u);
    }
}

static void opst_init_ends(void)
{
    for (uint32_t op = 0; op < EMBI_JSOP_COUNT; op++) {
        const char *n = embi_js_op_table[op].name;
        opst_end_tab[op] = strstr(n, "JMP") != nullptr || strcmp(n, "OPT_NULLISH") == 0 ||
                           strcmp(n, "FOR_IN_NEXT") == 0 || strcmp(n, "ITER_NEXT") == 0 ||
                           strcmp(n, "RETURN") == 0 || strcmp(n, "THROW") == 0 ||
                           strcmp(n, "RETHROW") == 0;
    }
    opst_end_init = true;
}

static void embi_js_opstat_step(const uint8_t *pc)
{
    uint8_t op = *pc;
    if (!opst_end_init) {
        opst_init_ends();
    }
    if (pc != opst_expect) {
        opst_h1 = OPST_NONE;
        opst_h2 = OPST_NONE;
    }
    opst_single[op]++;
    if (opst_h1 != OPST_NONE) {
        opst_pair[opst_h1 * 256u + op]++;
        if (opst_h2 != OPST_NONE) {
            opst_tri_add((opst_h2 << 16) | (opst_h1 << 8) | op);
        }
    }
    if (opst_end_tab[op]) {
        opst_h1 = OPST_NONE;
        opst_h2 = OPST_NONE;
    } else {
        opst_h2 = opst_h1;
        opst_h1 = op;
    }
    opst_expect = pc + embi_js_op_table[op].size;
}

void embi_js_opstats_reset(void)
{
    memset(opst_single, 0, sizeof opst_single);
    memset(opst_pair, 0, sizeof opst_pair);
    memset(opst_tri_key, 0, sizeof opst_tri_key);
    memset(opst_tri_cnt, 0, sizeof opst_tri_cnt);
    opst_expect = nullptr;
    opst_h1 = OPST_NONE;
    opst_h2 = OPST_NONE;
}

/* Appends TSV lines: "single<TAB>op<TAB>count", "pair<TAB>a<TAB>b<TAB>count" and
 * "triple<TAB>a<TAB>b<TAB>c<TAB>count". Zero counts are left out. */
void embi_js_opstats_dump(embi_sb *out)
{
    char line[256];
    for (uint32_t op = 0; op < EMBI_JSOP_COUNT; op++) {
        if (opst_single[op]) {
            int n = snprintf(line, sizeof line, "single\t%s\t%llu\n", embi_js_op_table[op].name,
                             (unsigned long long)opst_single[op]);
            embi_sb_append(out, (embi_str){line, (uint32_t)n});
        }
    }
    for (uint32_t a = 0; a < EMBI_JSOP_COUNT; a++) {
        for (uint32_t b = 0; b < EMBI_JSOP_COUNT; b++) {
            uint64_t c = opst_pair[a * 256u + b];
            if (c) {
                int n = snprintf(line, sizeof line, "pair\t%s\t%s\t%llu\n",
                                 embi_js_op_table[a].name, embi_js_op_table[b].name,
                                 (unsigned long long)c);
                embi_sb_append(out, (embi_str){line, (uint32_t)n});
            }
        }
    }
    for (uint32_t i = 0; i < OPST_TRI_SLOTS; i++) {
        if (opst_tri_key[i]) {
            uint32_t key = (uint32_t)(opst_tri_key[i] - 1u);
            int n = snprintf(line, sizeof line, "triple\t%s\t%s\t%s\t%llu\n",
                             embi_js_op_table[(key >> 16) & 255u].name,
                             embi_js_op_table[(key >> 8) & 255u].name,
                             embi_js_op_table[key & 255u].name,
                             (unsigned long long)opst_tri_cnt[i]);
            embi_sb_append(out, (embi_str){line, (uint32_t)n});
        }
    }
}
#endif


/* A direct eval call that reached the realm's eval: the source runs in the caller's scope. fp
 * points at the callee on the operand stack. Out of line so that the interpreter loop stays small. */
static __attribute__((noinline)) embi_js_value
direct_eval_exec(embi_js_runtime *rt, embi_js_vm *vm, const embi_js_code *code,
                 const embi_js_value *slots, const embi_js_value *upv, const embi_js_value *consts,
                 const embi_js_eval_site *es, embi_js_value *fp, const uint8_t *pc,
                 embi_js_value src)
{
    embi_js_frame *fr = &vm->frames.data[vm->frames.len - 1u];
    fr->sp = fp;
    fr->pc = pc + 5;
    embi_js_value ctx = build_context(rt, code, slots, upv, es);
    /* An arrow function reads this and new.target from the enclosing function's bindings. */
    embi_js_value this_v = fr->this_v;
    embi_js_value new_target = fr->new_target;
    if (es->this_up != EMBI_JS_NO_SLOT) {
        this_v = ((embi_js_box *)embi_js_as_cell(upv[es->this_up]))->value;
        new_target = ((embi_js_box *)embi_js_as_cell(upv[es->nt_up]))->value;
    }
    embi_js_value home = UNDEF;
    if (es->super_ok) {
        if (es->home_up != EMBI_JS_NO_SLOT) {
            home = ((embi_js_box *)embi_js_as_cell(upv[es->home_up]))->value;
        } else {
            embi_js_closure *cl = fr->callee ? closure_or_null(embi_js_from_object(fr->callee))
                                             : nullptr;
            home = cl ? cl_home(cl) : UNDEF;
        }
    }
    return eval_run(rt, vm->realm, src, ctx, es->strict != 0, this_v, new_target,
                    (es->nt_ok ? EMBI_JSC_EVAL_NT : 0u) | (es->in_params ? EMBI_JSC_EVAL_PARAMS : 0u) |
                        (es->super_ok ? EMBI_JSC_EVAL_SUPER : 0u) |
                        (es->priv_ok ? EMBI_JSC_EVAL_PRIV : 0u) |
                        (es->noargs ? EMBI_JSC_EVAL_NOARGS : 0u),
                    home, es->mod_k ? consts[es->mod_k - 1u] : EMBI_JS_EMPTY);
}

/* eval(...args): the operands are [fn this args]; returns the call's result or EXC. */
static __attribute__((noinline)) embi_js_value
eval_spread_exec(embi_js_runtime *rt, embi_js_vm *vm, const embi_js_code *code,
                 const embi_js_value *slots, const embi_js_value *upv, const embi_js_value *consts,
                 const embi_js_eval_site *es, embi_js_value *fp, const uint8_t *pc)
{
    uint32_t argc = 0;
    embi_js_value *argv = nullptr;
    if (!spread_list(rt, fp[2], &argc, &argv)) {
        return EXC;
    }
    if (interrupted(rt)) {
        spread_free(argv, argc);
        vm->terminating = true;
        (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
        return EXC;
    }
    if (embi_js_is_object(fp[0]) && embi_js_as_object(fp[0]) == vm->realm->eval_fn) {
        embi_js_value src = argc > 0 ? argv[0] : UNDEF;
        spread_free(argv, argc);
        return direct_eval_exec(rt, vm, code, slots, upv, consts, es, fp, pc, src);
    }
    embi_js_frame *fr = &vm->frames.data[vm->frames.len - 1u];
    fr->sp = fp;
    fr->pc = pc + 5;
    if (!embi_js_is_callable(fp[0])) {
        spread_free(argv, argc);
        fr->sp = fp + 3;
        return embi_js_throw_type_error(rt, "expression is not a function");
    }
    embi_js_value r = embi_js_call(rt, fp[0], fp[1], (int)argc, argv);
    spread_free(argv, argc);
    return r;
}

static embi_js_value run(embi_js_runtime *rt)
{
    embi_js_vm *vm = rt->vm;
#define FRAME (&vm->frames.data[vm->frames.len - 1u])
    embi_js_code *code;
    const uint8_t *pc;
    embi_js_value *sp;
    embi_js_value *slots;
    embi_js_value *consts;
    embi_js_value *upv;
    bool rethrown = false;
    uint32_t call_operand = 0;

#define LOAD_FRAME()                                                                           \
    do {                                                                                       \
        code = FRAME->code;                                                                       \
        if (code->ics == nullptr && code->nics != 0) {                                            \
            embi_js_code_ics_ensure(rt, code);                                                 \
        }                                                                                      \
        pc = FRAME->pc;                                                                           \
        sp = FRAME->sp;                                                                           \
        slots = FRAME->base;                                                                      \
        consts = code->consts;                                                                 \
        upv = closure_upvals(FRAME->callee);                                                      \
    } while (0)

#define OPERAND(n) embi_js_rd32(pc + (n))
#ifdef EMBI_JS_OPSTATS
#define OPSTAT_STEP(p) embi_js_opstat_step(p)
#else
#define OPSTAT_STEP(p) ((void)0)
#endif

#if EMBI_JS_COMPUTED_GOTO
    static void *const dispatch_table[] = {
#define OP(name, size, pops, pushes) &&L_##name,
#include "js/opcodes.def"
#undef OP
    };
#define CASE(op) L_##op:
#define NEXT() do { OPSTAT_STEP(pc); vm->pc_cur = pc; goto *dispatch_table[*pc]; } while (0)
#else
#define CASE(op) case EMBI_JSOP_##op:
#define NEXT() goto dispatch_top
#endif
#define TRUTHY(v)                                                                              \
    ((v) == EMBI_JS_TRUE    ? true                                                              \
     : (v) == EMBI_JS_FALSE ? false                                                             \
     : embi_js_is_int32(v)  ? embi_js_as_int32(v) != 0                                          \
                            : embi_js_to_boolean(v))
/* Result of a two-operand op at sp[-2], sp[-1]: pops one slot, advances past the op. */
#define BIN_RESULT(v)                                                                          \
    do {                                                                                       \
        sp--;                                                                                  \
        sp[-1] = (v);                                                                          \
        pc += 1;                                                                               \
        NEXT();                                                                                \
    } while (0)
#define ARITH_SLOW(opc)                                                                        \
    do {                                                                                       \
        embi_js_value r_ = op_arith(rt, (opc), sp[-2], sp[-1]);                                \
        if (r_ == EXC) {                                                                       \
            goto throw_;                                                                       \
        }                                                                                      \
        BIN_RESULT(r_);                                                                        \
    } while (0)
#define BITS_SLOW(opc)                                                                         \
    do {                                                                                       \
        embi_js_value r_ = op_bits(rt, (opc), sp[-2], sp[-1]);                                 \
        if (r_ == EXC) {                                                                       \
            goto throw_;                                                                       \
        }                                                                                      \
        BIN_RESULT(r_);                                                                        \
    } while (0)
#define CMP_SLOW(opc)                                                                          \
    do {                                                                                       \
        embi_js_value r_ = op_compare(rt, (opc), sp[-2], sp[-1]);                              \
        if (r_ == EXC) {                                                                       \
            goto throw_;                                                                       \
        }                                                                                      \
        BIN_RESULT(r_);                                                                        \
    } while (0)

    LOAD_FRAME();
    NEXT();

#if !EMBI_JS_COMPUTED_GOTO
dispatch_top:
    OPSTAT_STEP(pc);
    vm->pc_cur = pc;
    switch ((enum embi_js_op)*pc) {
#endif

    CASE(NOP) {
        pc += 1;
        NEXT();
    }
    CASE(POP) {
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(DUP) {
        sp[0] = sp[-1];
        sp++;
        pc += 1;
        NEXT();
    }
    CASE(DUP2) {
        sp[0] = sp[-2];
        sp[1] = sp[-1];
        sp += 2;
        pc += 1;
        NEXT();
    }
    CASE(ROT3) {
        embi_js_value c = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = sp[-3];
        sp[-3] = c;
        pc += 1;
        NEXT();
    }
    CASE(ROT4) {
        embi_js_value d = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = sp[-3];
        sp[-3] = sp[-4];
        sp[-4] = d;
        pc += 1;
        NEXT();
    }
    CASE(SWAP) {
        embi_js_value t = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = t;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_UNDEF) {
        *sp++ = UNDEF;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_NULL) {
        *sp++ = EMBI_JS_NULL;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_TRUE) {
        *sp++ = EMBI_JS_TRUE;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_FALSE) {
        *sp++ = EMBI_JS_FALSE;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_I32) {
        *sp++ = embi_js_from_int32((int32_t)OPERAND(1));
        pc += 5;
        NEXT();
    }
    CASE(PUSH_CONST) {
        *sp++ = consts[OPERAND(1)];
        pc += 5;
        NEXT();
    }
    CASE(NEW_REGEXP) {
        embi_js_value re = embi_js_regexp_from_template(rt, consts[OPERAND(1)]);
        if (re == EMBI_JS_EXCEPTION) {
            goto throw_;
        }
        *sp++ = re;
        pc += 5;
        NEXT();
    }
    CASE(PUSH_THIS) {
        *sp++ = FRAME->this_v;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_CALLEE) {
        *sp++ = FRAME->callee ? embi_js_from_object(FRAME->callee) : UNDEF;
        pc += 1;
        NEXT();
    }
    CASE(CONST_FALSE) {
        *sp++ = EMBI_JS_FALSE;
        pc += 5;
        NEXT();
    }
    CASE(GET_LOC) {
        *sp++ = slots[OPERAND(1)];
        pc += 5;
        NEXT();
    }
    CASE(SET_LOC) {
        slots[OPERAND(1)] = sp[-1];
        pc += 5;
        NEXT();
    }
    CASE(PUT_LOC) {
        slots[OPERAND(1)] = *--sp;
        pc += 5;
        NEXT();
    }
    CASE(GET_BOX) {
        *sp++ = ((embi_js_box *)embi_js_as_cell(slots[OPERAND(1)]))->value;
        pc += 5;
        NEXT();
    }
    CASE(SET_BOX) {
        embi_js_box *b = (embi_js_box *)embi_js_as_cell(slots[OPERAND(1)]);
        b->value = sp[-1];
        EMBI_JS_WB(b, sp[-1]);
        pc += 5;
        NEXT();
    }
    CASE(INIT_BOX) {
        embi_js_value v = *--sp;
        embi_js_box *b = embi_js_box_new(rt, v);
        slots[OPERAND(1)] = embi_js_from_cell(b);
        pc += 5;
        NEXT();
    }
    CASE(PUT_BOX) {
        embi_js_box *b = (embi_js_box *)embi_js_as_cell(slots[OPERAND(1)]);
        b->value = *--sp;
        EMBI_JS_WB(b, b->value);
        pc += 5;
        NEXT();
    }
    CASE(MAKE_BOX) {
        uint32_t slot = OPERAND(1);
        embi_js_value cur = slots[slot];
        /* A parameter of a mapped arguments object is boxed when the frame is pushed. */
        if (!(embi_js_is_cell(cur) && embi_js_as_cell(cur)->type == EMBI_JS_CELL_BOX)) {
            slots[slot] = embi_js_from_cell(embi_js_box_new(rt, cur));
        }
        pc += 5;
        NEXT();
    }
    CASE(GET_UPVAL) {
        *sp++ = ((embi_js_box *)embi_js_as_cell(upv[OPERAND(1)]))->value;
        pc += 5;
        NEXT();
    }
    CASE(SET_UPVAL) {
        embi_js_box *b = (embi_js_box *)embi_js_as_cell(upv[OPERAND(1)]);
        b->value = sp[-1];
        EMBI_JS_WB(b, sp[-1]);
        pc += 5;
        NEXT();
    }
    CASE(IMPORT_GET) {
        embi_js_value v = embi_jsm_import_get(rt, consts[OPERAND(1)]);
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(IMPORT_META) {
        embi_js_value v = embi_jsm_import_meta(rt, consts[OPERAND(1)]);
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(DYN_IMPORT) {
        embi_js_value v = embi_jsm_dynamic_import(rt, consts[OPERAND(1)], sp[-2], sp[-1]);
        sp -= 2;
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(GET_GLOBAL) {
        embi_js_value v = global_get_ic(rt, (embi_js_cell *)code, &code->ics[OPERAND(1)], false);
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(GET_GLOBAL_OPT) {
        embi_js_value v = global_get_ic(rt, (embi_js_cell *)code, &code->ics[OPERAND(1)], true);
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(SET_GLOBAL) {
        embi_js_value v = global_set_ic(rt, (embi_js_cell *)code, &code->ics[OPERAND(1)], sp[-1],
                                        (code->flags & EMBI_JS_CODE_STRICT) != 0);
        if (v == EXC) {
            goto throw_;
        }
        pc += 5;
        NEXT();
    }
    CASE(DELETE_GLOBAL) {
        embi_js_object *g = global_of(rt);
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        bool lc;
        if (lex_find(rt, k, &lc)) {
            *sp++ = EMBI_JS_FALSE;
            pc += 5;
            NEXT();
        }
        int r = embi_js_obj_delete(rt, g, k);
        if (r == EMBI_JS_THREW) {
            goto throw_;
        }
        if (r == EMBI_JS_YES) {
            embi_js_remove_raw(rt, vm->realm->vars, k);
        }
        *sp++ = embi_js_from_bool(r == EMBI_JS_YES);
        pc += 5;
        NEXT();
    }
    CASE(DECLARE_GLOBAL) {
        embi_js_object *g = global_of(rt);
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        embi_js_pdesc d;
        if (embi_js_obj_get_own_property(rt, g, k, &d) == EMBI_JS_NO) {
            embi_js_define_raw(rt, g, k, UNDEF, ATTR_W | ATTR_E);
        }
        var_names_add(rt, k);
        pc += 5;
        NEXT();
    }
    CASE(GLOBAL_CHECK) {
        embi_js_value name = consts[OPERAND(1)];
        uint32_t kind = OPERAND(5);
        embi_js_object *g = global_of(rt);
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(name));
        bool lc;
        embi_js_pdesc d;
        int own = embi_js_obj_get_own_property(rt, g, k, &d);
        if (own == EMBI_JS_THREW) {
            goto throw_;
        }
        bool clash = lex_find(rt, k, &lc) != nullptr;
        if (kind == 0) {
            clash = clash || embi_js_obj_has_own_property(rt, vm->realm->vars, k) == EMBI_JS_YES ||
                    (own == EMBI_JS_YES && !(d.attrs & EMBI_JS_ATTR_CONFIGURABLE));
        }
        if (clash) {
            (void)throw_name(rt, EMBI_JS_PROTO_SYNTAX_ERROR, "Identifier '", name,
                             "' has already been declared");
            goto throw_;
        }
        if (kind == 1 && own == EMBI_JS_NO && !embi_js_obj_is_extensible(g)) {
            (void)throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot declare global variable '", name,
                             "': the global object is not extensible");
            goto throw_;
        }
        if (kind == 2) {
            bool ok = own == EMBI_JS_NO ? embi_js_obj_is_extensible(g)
                                        : ((d.attrs & EMBI_JS_ATTR_CONFIGURABLE) ||
                                           (!embi_js_pd_is_accessor(&d) &&
                                            (d.attrs & EMBI_JS_ATTR_WRITABLE) &&
                                            (d.attrs & EMBI_JS_ATTR_ENUMERABLE)));
            if (!ok) {
                (void)throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot declare global function '",
                                 name, "'");
                goto throw_;
            }
        }
        pc += 9;
        NEXT();
    }
    CASE(DECLARE_GLEX) {
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        embi_js_box *b = embi_js_box_new(rt, EMBI_JS_EMPTY);
        embi_js_define_raw(rt, vm->realm->lex, k, embi_js_from_cell(b),
                           OPERAND(5) ? 0 : EMBI_JS_ATTR_WRITABLE);
        vm->realm->nlex++;
        pc += 9;
        NEXT();
    }
    CASE(INIT_GLEX) {
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        bool lc;
        embi_js_box *b = lex_find(rt, k, &lc);
        b->value = *--sp;
        EMBI_JS_WB(b, b->value);
        pc += 5;
        NEXT();
    }
    CASE(DEFINE_GLOBAL_FN) {
        embi_js_object *g = global_of(rt);
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        embi_js_value v = *--sp;
        embi_js_pdesc d;
        var_names_add(rt, k);
        int own = embi_js_obj_get_own_property(rt, g, k, &d);
        if (own == EMBI_JS_NO || (d.attrs & EMBI_JS_ATTR_CONFIGURABLE)) {
            /* CreateGlobalFunctionBinding: a new or configurable property is (re)defined. */
            embi_js_define_raw(rt, g, k, v, ATTR_W | ATTR_E);
        } else {
            int r = embi_js_put(rt, g, k, v, false);
            if (r == EMBI_JS_THREW) {
                goto throw_;
            }
        }
        pc += 5;
        NEXT();
    }
    CASE(SYM_GET)
    CASE(SYM_SET)
    CASE(SYM_INIT)
    CASE(SYM_TYPEOF)
    CASE(SYM_CALL)
    CASE(SYM_REF)
    CASE(SYM_GETREF)
    CASE(SYM_SETREF)
    CASE(SYM_DELETE) {
        EMBI_UNREACHABLE();
        NEXT();
    }
    CASE(GET_FIELD) {
        embi_js_value base = sp[-1];
        embi_js_ic *ic = &code->ics[OPERAND(1)];
        embi_js_value v;
        if (!embi_js_is_object(base)) {
            v = get_field_prim(rt, (embi_js_cell *)code, ic, base);
        } else if (!embi_js_ic_get_inline(ic, embi_js_as_object(base), &v) &&
                   !embi_js_ic_mega_get_inline(rt, ic, embi_js_as_object(base), &v)) {
            v = embi_js_ic_get(rt, (embi_js_cell *)code, ic, embi_js_as_object(base), base);
        }
        if (v == EXC) {
            goto throw_;
        }
        sp[-1] = v;
        pc += 5;
        NEXT();
    }
    CASE(SET_FIELD) {
        embi_js_value base = sp[-2];
        embi_js_value v = sp[-1];
        embi_js_ic *ic = &code->ics[OPERAND(1)];
        bool strict = (code->flags & EMBI_JS_CODE_STRICT) != 0;
        if (embi_js_is_object(base)) {
            embi_js_object *o = embi_js_as_object(base);
            if (!embi_js_ic_put_inline(ic, o, v) && !embi_js_ic_mega_put_inline(rt, ic, o, v) &&
                embi_js_ic_put(rt, (embi_js_cell *)code, ic, o, v, strict) == EMBI_JS_THREW) {
                goto throw_;
            }
        } else if (put_prop(rt, base, ic->key, v, strict) == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp--;
        pc += 5;
        NEXT();
    }
    CASE(TO_PROPKEY) {
        if (embi_js_is_object(sp[-1])) {
            embi_js_key k = embi_js_to_property_key(rt, sp[-1]);
            if (k == EXC) {
                goto throw_;
            }
            sp[-1] = embi_js_key_to_value(rt, k);
        }
        pc += 1;
        NEXT();
    }
    CASE(CHECK_BASE) {
        embi_js_value base = sp[-2];
        if (embi_js_is_nullish(base)) {
            if (embi_js_throw_type_error(rt, "Cannot read properties of %s",
                                         base == EMBI_JS_NULL ? "null" : "undefined") == EXC) {
                goto throw_;
            }
        }
        pc += 1;
        NEXT();
    }
    CASE(GET_INDEX) {
        embi_js_value base = sp[-2];
        uint32_t ui = 0;
        embi_js_value fv = EMBI_JS_UNDEFINED;
        if (embi_js_is_object(base) && index_key_u32(sp[-1], &ui) &&
            embi_js_obj_index_get_fast(rt, embi_js_as_object(base), ui, &fv)) {
            sp[-2] = fv;
            sp--;
            pc += 1;
            NEXT();
        }
        if (embi_js_is_nullish(base)) {
            if (embi_js_throw_type_error(rt, "Cannot read properties of %s",
                                         base == EMBI_JS_NULL ? "null" : "undefined") == EXC) {
                goto throw_;
            }
        }
        embi_js_key k = embi_js_to_property_key(rt, sp[-1]);
        if (k == EXC) {
            goto throw_;
        }
        /* A canonical index string (\"3\") is the index key of the same element (J5e). */
        if (embi_js_is_object(base) && embi_js_key_is_index(k) &&
            embi_js_obj_index_get_fast(rt, embi_js_as_object(base), embi_js_key_index(k), &fv)) {
            sp[-2] = fv;
            sp--;
            pc += 1;
            NEXT();
        }
        embi_js_value v = get_prop(rt, base, k);
        if (v == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(SET_INDEX) {
        embi_js_value base = sp[-3];
        embi_js_value v = sp[-1];
        uint32_t ui = 0;
        if (embi_js_is_object(base) && index_key_u32(sp[-2], &ui) &&
            embi_js_obj_index_set_fast(rt, embi_js_as_object(base), ui, v)) {
            sp[-3] = v;
            sp -= 2;
            pc += 1;
            NEXT();
        }
        if (embi_js_is_nullish(base)) {
            (void)embi_js_throw_type_error(rt, "Cannot set properties of %s",
                                           base == EMBI_JS_NULL ? "null" : "undefined");
            goto throw_;
        }
        embi_js_key k = embi_js_to_property_key(rt, sp[-2]);
        if (k == EXC) {
            goto throw_;
        }
        if (embi_js_is_object(base) && embi_js_key_is_index(k) &&
            embi_js_obj_index_set_fast(rt, embi_js_as_object(base), embi_js_key_index(k), v)) {
            sp[-3] = v;
            sp -= 2;
            pc += 1;
            NEXT();
        }
        if (put_prop(rt, base, k, v, (code->flags & EMBI_JS_CODE_STRICT) != 0) == EXC) {
            goto throw_;
        }
        sp[-3] = v;
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(GET_FIELD_THIS) {
        embi_js_value base = sp[-1];
        embi_js_ic *ic = &code->ics[OPERAND(1)];
        embi_js_value v;
        if (!embi_js_is_object(base)) {
            v = get_field_prim(rt, (embi_js_cell *)code, ic, base);
        } else if (!embi_js_ic_get_inline(ic, embi_js_as_object(base), &v) &&
                   !embi_js_ic_mega_get_inline(rt, ic, embi_js_as_object(base), &v)) {
            v = embi_js_ic_get(rt, (embi_js_cell *)code, ic, embi_js_as_object(base), base);
        }
        if (v == EXC) {
            goto throw_;
        }
        sp[-1] = v;
        *sp++ = base;
        pc += 5;
        NEXT();
    }
    CASE(GET_INDEX_THIS) {
        embi_js_value base = sp[-2];
        uint32_t ui = 0;
        embi_js_value fv = EMBI_JS_UNDEFINED;
        if (embi_js_is_object(base) && index_key_u32(sp[-1], &ui) &&
            embi_js_obj_index_get_fast(rt, embi_js_as_object(base), ui, &fv)) {
            sp[-2] = fv;
            sp[-1] = base;
            pc += 1;
            NEXT();
        }
        if (embi_js_is_nullish(base)) {
            (void)embi_js_throw_type_error(rt, "Cannot read properties of %s",
                                           base == EMBI_JS_NULL ? "null" : "undefined");
            goto throw_;
        }
        embi_js_key k = embi_js_to_property_key(rt, sp[-1]);
        if (k == EXC) {
            goto throw_;
        }
        if (embi_js_is_object(base) && embi_js_key_is_index(k) &&
            embi_js_obj_index_get_fast(rt, embi_js_as_object(base), embi_js_key_index(k), &fv)) {
            sp[-2] = fv;
            sp[-1] = base;
            pc += 1;
            NEXT();
        }
        embi_js_value v = get_prop(rt, base, k);
        if (v == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp[-1] = base;
        pc += 1;
        NEXT();
    }
    CASE(DELETE_FIELD) {
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        embi_js_value v = delete_prop(rt, sp[-1], k, (code->flags & EMBI_JS_CODE_STRICT) != 0);
        if (v == EXC) {
            goto throw_;
        }
        sp[-1] = v;
        pc += 5;
        NEXT();
    }
    CASE(DELETE_INDEX) {
        embi_js_key k = embi_js_to_property_key(rt, sp[-1]);
        if (k == EXC) {
            goto throw_;
        }
        embi_js_value v = delete_prop(rt, sp[-2], k, (code->flags & EMBI_JS_CODE_STRICT) != 0);
        if (v == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(DEFINE_FIELD) {
        embi_js_object *o = embi_js_as_object(sp[-2]);
        if (embi_js_ic_define(rt, (embi_js_cell *)code, &code->ics[OPERAND(1)], o, sp[-1]) ==
            EMBI_JS_THREW) {
            goto throw_;
        }
        sp--;
        pc += 5;
        NEXT();
    }
    CASE(DEFINE_GETTER)
    CASE(DEFINE_SETTER) {
        embi_js_object *o = embi_js_as_object(sp[-2]);
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        bool getter = pc[0] == EMBI_JSOP_DEFINE_GETTER;
        embi_js_pdesc d;
        memset(&d, 0, sizeof d);
        d.value = UNDEF;
        d.get = UNDEF;
        d.set = UNDEF;
        d.attrs = ATTR_E | ATTR_C;
        if (getter) {
            d.has = EMBI_JS_PD_GET;
            d.get = sp[-1];
        } else {
            d.has = EMBI_JS_PD_SET;
            d.set = sp[-1];
        }
        d.has |= EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;
        if (embi_js_obj_define_own_property(rt, o, k, &d) == EMBI_JS_THREW) {
            goto throw_;
        }
        sp--;
        pc += 5;
        NEXT();
    }
    CASE(DEFINE_INDEX) {
        embi_js_object *o = embi_js_as_object(sp[-2]);
        if (!embi_js_obj_create_index_fast(rt, o, OPERAND(1), sp[-1]) &&
            embi_js_create_data_property(rt, o, embi_js_key_from_index(OPERAND(1)), sp[-1]) ==
                EMBI_JS_THREW) {
            goto throw_;
        }
        sp--;
        pc += 5;
        NEXT();
    }
    CASE(NEW_OBJECT) {
        *sp++ = embi_js_from_object(embi_js_object_new_plain(rt));
        pc += 1;
        NEXT();
    }
    CASE(NEW_ARRAY) {
        embi_js_object *a = embi_js_array_create_default(rt, (double)OPERAND(1));
        if (!a) {
            goto throw_;
        }
        embi_js_array_reserve(rt, a, OPERAND(1) < 1024u ? OPERAND(1) : 1024u);
        *sp++ = embi_js_from_object(a);
        pc += 5;
        NEXT();
    }
    CASE(NEW_CLOSURE) {
        embi_js_object *f = make_closure(rt, consts[OPERAND(1)], slots, FRAME->callee);
        *sp++ = embi_js_from_object(f);
        pc += 5;
        NEXT();
    }
    CASE(CALL) {
        call_operand = OPERAND(1);
    }
    do_call: {
        uint32_t argc = EMBI_JS_CALL_ARGC(call_operand);
        embi_js_value *fp = sp - argc - 2;
        embi_js_value fn = fp[0];
        embi_js_value thisv = fp[1];
        const embi_js_value *argv = fp + 2;
        if (interrupted(rt)) {
            vm->terminating = true;
            (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
            goto throw_;
        }
        FRAME->sp = fp;
        FRAME->pc = pc + 5;
        if (embi_js_is_object(fn) && embi_js_as_object(fn)->klass == &embi_js_class_closure &&
            !(code_of(closure_of(embi_js_as_object(fn))->code)->flags & EMBI_JS_CODE_CLASS_CTOR)) {
            embi_js_object *fo = embi_js_as_object(fn);
            if (!push_frame(rt, fo, code_of(closure_of(fo)->code), thisv, (int)argc, argv, UNDEF, 0)) {
                FRAME->sp = sp;
                goto throw_;
            }
            LOAD_FRAME();
            NEXT();
        }
        if (!embi_js_is_callable(fn)) {
            uint32_t name_k = EMBI_JS_CALL_NAME(call_operand);
            FRAME->sp = sp;
            if (name_k != 0) {
                (void)throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "", consts[name_k - 1u],
                                 " is not a function");
            } else {
                (void)embi_js_throw_type_error(rt, "expression is not a function");
            }
            goto throw_;
        }
        /* The native callee may re-enter the interpreter, which reads this frame's resume state. */
        embi_js_value r = embi_js_call(rt, fn, thisv, (int)argc, argv);
        if (r == EXC) {
            goto throw_;
        }
        sp = fp;
        *sp++ = r;
        pc += 5;
        NEXT();
    }
    CASE(NEW) {
        uint32_t argc = OPERAND(1);
        embi_js_value *fp = sp - argc - 1;
        embi_js_value fn = fp[0];
        const embi_js_value *argv = fp + 1;
        if (interrupted(rt)) {
            vm->terminating = true;
            (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
            goto throw_;
        }
        if (embi_js_is_object(fn) && embi_js_as_object(fn)->klass == &embi_js_class_closure &&
            (code_of(closure_of(embi_js_as_object(fn))->code)->flags & EMBI_JS_CODE_CONSTRUCTOR) &&
            !(code_of(closure_of(embi_js_as_object(fn))->code)->flags & EMBI_JS_CODE_DERIVED)) {
            embi_js_object *fo = embi_js_as_object(fn);
            embi_js_value proto = embi_js_proto_from_ctor(rt, fn, EMBI_JS_PROTO_OBJECT);
            if (proto == EXC) {
                goto throw_;
            }
            embi_js_object *self = embi_js_object_new(rt, proto);
            if (!class_fields_init(rt, fn, embi_js_from_object(self))) {
                goto throw_;
            }
            FRAME->sp = fp;
            FRAME->pc = pc + 5;
            if (!push_frame(rt, fo, code_of(closure_of(fo)->code), embi_js_from_object(self),
                            (int)argc, argv, fn, EMBI_JS_FRAME_CONSTRUCT)) {
                FRAME->sp = sp;
                goto throw_;
            }
            LOAD_FRAME();
            NEXT();
        }
        FRAME->sp = fp;
        FRAME->pc = pc + 5;
        embi_js_value r = embi_js_construct(rt, fn, (int)argc, argv, fn);
        if (r == EXC) {
            goto throw_;
        }
        sp = fp;
        *sp++ = r;
        pc += 5;
        NEXT();
    }
    CASE(CALL_EVAL) {
        const embi_js_eval_site *es = &code->esites[OPERAND(1)];
        uint32_t argc = es->argc;
        embi_js_value *fp = sp - argc - 2;
        if (embi_js_is_object(fp[0]) && embi_js_as_object(fp[0]) == vm->realm->eval_fn) {
            if (interrupted(rt)) {
                vm->terminating = true;
                (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
                goto throw_;
            }
            embi_js_value src = argc > 0 ? fp[2] : UNDEF;
            embi_js_value r = direct_eval_exec(rt, vm, code, slots, upv, consts, es, fp, pc, src);
            if (r == EXC) {
                goto throw_;
            }
            sp = fp;
            *sp++ = r;
            pc += 5;
            NEXT();
        }
        call_operand = argc | (es->name_k << 16);
        goto do_call;
    }
    CASE(CALL_EVAL_SPREAD) {
        /* eval(...args): [fn this args] -> [result]. The first spread value is the source. */
        embi_js_value *fp = sp - 3;
        embi_js_value r = eval_spread_exec(rt, vm, code, slots, upv, consts,
                                           &code->esites[OPERAND(1)], fp, pc);
        if (r == EXC) {
            goto throw_;
        }
        sp = fp;
        *sp++ = r;
        pc += 5;
        NEXT();
    }
    CASE(DYN_GET)
    CASE(DYN_TYPEOF) {
        const embi_js_dyn_site *s = &code->dsites[OPERAND(1)];
        dref r = dref_new(rt, code, s);
        int lr = dyn_lookup(rt, code, slots, upv, s, &r);
        if (lr == EMBI_JS_THREW) {
            goto throw_;
        }
        if (lr == EMBI_JS_NO) {
            pc = code->bc + s->fallback;
            NEXT();
        }
        embi_js_value v = dref_get(rt, &r, (code->flags & EMBI_JS_CODE_STRICT) != 0);
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(DYN_SET) {
        const embi_js_dyn_site *s = &code->dsites[OPERAND(1)];
        dref r = dref_new(rt, code, s);
        int lr = dyn_lookup(rt, code, slots, upv, s, &r);
        if (lr == EMBI_JS_THREW) {
            goto throw_;
        }
        if (lr == EMBI_JS_NO) {
            pc = code->bc + s->fallback;
            NEXT();
        }
        if (dref_put(rt, &r, sp[-1], (code->flags & EMBI_JS_CODE_STRICT) != 0) == EMBI_JS_THREW) {
            goto throw_;
        }
        pc += 5;
        NEXT();
    }
    CASE(DYN_REF) {
        const embi_js_dyn_site *s = &code->dsites[OPERAND(1)];
        dref r = dref_new(rt, code, s);
        int lr = dyn_lookup(rt, code, slots, upv, s, &r);
        if (lr == EMBI_JS_THREW) {
            goto throw_;
        }
        if (lr == EMBI_JS_NO) {
            embi_js_value hold = s->unres ? dyn_ref_unres(rt, r.key) : UNDEF;
            if (hold == EXC) {
                goto throw_;
            }
            *sp++ = hold;
        } else if (r.box && r.ro) {
            /* A read-only name (an import of a module, seen through an eval context): the handle is
             * a snapshot box that refuses stores. The reference is read at once, so the snapshot is
             * the value the read sees; the exporter's own box is never marked. */
            embi_js_box *nb = embi_js_box_new(rt, r.box->value);
            nb->hdr.aux = 1;
            *sp++ = embi_js_from_cell(nb);
        } else {
            *sp++ = r.box ? embi_js_from_cell(r.box) : embi_js_from_object(r.obj);
        }
        pc += 5;
        NEXT();
    }
    CASE(DYN_GET_REF) {
        embi_js_value h = sp[-1];
        if (h == UNDEF || h == EMBI_JS_NULL) {
            pc = code->bc + code->dsites[OPERAND(1)].fallback;
            NEXT();
        }
        embi_js_value v;
        if (embi_js_is_object(h)) {
            dref r = dref_new(rt, code, &code->dsites[OPERAND(1)]);
            r.obj = embi_js_as_object(h);
            v = dref_get(rt, &r, (code->flags & EMBI_JS_CODE_STRICT) != 0);
        } else {
            v = ((embi_js_box *)embi_js_as_cell(h))->value;
        }
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(DYN_SET_REF) {
        embi_js_value h = sp[-2];
        embi_js_value v = sp[-1];
        if (h == EMBI_JS_NULL && code->bc[code->dsites[OPERAND(1)].fallback] == EMBI_JSOP_SET_GLOBAL) {
            /* The name was unresolvable when the reference was made (see DYN_REF). */
            (void)dyn_set_unres_throw(rt, code, &code->dsites[OPERAND(1)]);
            goto throw_;
        }
        if (h == UNDEF || h == EMBI_JS_NULL) {
            sp[-2] = v;
            sp--;
            pc = code->bc + code->dsites[OPERAND(1)].fallback;
            NEXT();
        }
        if (embi_js_is_object(h)) {
            dref r = dref_new(rt, code, &code->dsites[OPERAND(1)]);
            r.obj = embi_js_as_object(h);
            if (dref_put(rt, &r, v, (code->flags & EMBI_JS_CODE_STRICT) != 0) == EMBI_JS_THREW) {
                goto throw_;
            }
        } else {
            embi_js_box *bx = (embi_js_box *)embi_js_as_cell(h);
            if (box_is_ro(bx)) {
                if (box_store_ro(rt, (code->flags & EMBI_JS_CODE_STRICT) != 0) == EMBI_JS_THREW) {
                    goto throw_;
                }
                sp[-2] = v;
                sp--;
                pc += 5;
                NEXT();
            }
            bx->value = v;
            EMBI_JS_WB(bx, v);
        }
        sp[-2] = v;
        sp--;
        pc += 5;
        NEXT();
    }
    CASE(DYN_CALL) {
        const embi_js_dyn_site *s = &code->dsites[OPERAND(1)];
        dref r = dref_new(rt, code, s);
        int lr = dyn_lookup(rt, code, slots, upv, s, &r);
        if (lr == EMBI_JS_THREW) {
            goto throw_;
        }
        if (lr == EMBI_JS_NO) {
            pc = code->bc + s->fallback;
            NEXT();
        }
        embi_js_value v = dref_get(rt, &r, (code->flags & EMBI_JS_CODE_STRICT) != 0);
        if (v == EXC) {
            goto throw_;
        }
        *sp++ = v;
        *sp++ = r.obj ? embi_js_from_object(r.obj) : UNDEF;
        pc += 6;
        NEXT();
    }
    CASE(DYN_DELETE) {
        const embi_js_dyn_site *s = &code->dsites[OPERAND(1)];
        dref r = dref_new(rt, code, s);
        int lr = dyn_lookup(rt, code, slots, upv, s, &r);
        if (lr == EMBI_JS_THREW) {
            goto throw_;
        }
        if (lr == EMBI_JS_NO) {
            pc = code->bc + s->fallback;
            NEXT();
        }
        if (r.obj) {
            int d = embi_js_obj_delete(rt, r.obj, r.key);
            if (d == EMBI_JS_THREW) {
                goto throw_;
            }
            *sp++ = embi_js_from_bool(d == EMBI_JS_YES);
        } else if (r.deletable) {
            embi_js_remove_raw(rt, r.rec, r.key);
            *sp++ = embi_js_from_bool(true);
        } else {
            *sp++ = embi_js_from_bool(false);
        }
        pc += 5;
        NEXT();
    }
    CASE(EVAL_CHECK) {
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        if (eval_check(rt, upv, k, OPERAND(5)) == EMBI_JS_THREW) {
            goto throw_;
        }
        pc += 9;
        NEXT();
    }
    CASE(EVAL_DECL) {
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        if (eval_bind(rt, upv, k, false, UNDEF) == EMBI_JS_THREW) {
            goto throw_;
        }
        pc += 5;
        NEXT();
    }
    CASE(EVAL_DEFINE) {
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        embi_js_value v = *--sp;
        if (eval_bind(rt, upv, k, true, v) == EMBI_JS_THREW) {
            goto throw_;
        }
        pc += 5;
        NEXT();
    }
    CASE(NEW_ENV) {
        *sp++ = embi_js_from_object(embi_js_object_new(rt, EMBI_JS_NULL));
        pc += 1;
        NEXT();
    }
    CASE(TO_OBJECT) {
        embi_js_value v = sp[-1];
        if (embi_js_is_nullish(v)) {
            (void)embi_js_throw_type_error(rt, "Cannot convert undefined or null to object");
            goto throw_;
        }
        embi_js_object *o = embi_js_to_object(rt, v);
        if (!o) {
            goto throw_;
        }
        sp[-1] = embi_js_from_object(o);
        pc += 1;
        NEXT();
    }
    CASE(RETURN) {
        embi_js_value v = sp[-1];
        if ((FRAME->flags & EMBI_JS_FRAME_CONSTRUCT) && !embi_js_is_object(v)) {
            v = FRAME->this_v;
        }
        if (FRAME->flags & EMBI_JS_FRAME_ASYNC) {
            v = async_settle(rt, FRAME->gen, v, false);
        }
        bool entry = (FRAME->flags & EMBI_JS_FRAME_ENTRY) != 0;
        pop_frame(vm);
        if (entry) {
            return v;
        }
        LOAD_FRAME();
        *sp++ = v;
        NEXT();
    }
    CASE(THROW) {
        embi_js_exception_set(rt, sp[-1]);
        sp--;
        goto throw_;
    }
    CASE(RETHROW) {
        embi_js_exception_set(rt, sp[-1]);
        sp--;
        restore_location(vm, rt->pending_exception);
        rethrown = true;
        goto throw_;
    }
    CASE(JMP) {
        uint32_t target = OPERAND(1);
        if (target <= (uint32_t)(pc - code->bc)) {
            if (interrupted(rt)) {
                vm->terminating = true;
                (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
                goto throw_;
            }
        }
        pc = code->bc + target;
        NEXT();
    }
    CASE(JMP_ENTRY) {
        pc = code->bc + OPERAND(1);
        NEXT();
    }
    CASE(SWITCH_JMP) {
        /* CF1: the constant case tests of a switch (opcodes.def), compared with sp[-1] as SEQ would. The chain only
         * moves forward, so it ends; a test of another form, or the op after the last test, runs as usual. */
        const uint8_t *q = code->bc + OPERAND(1);
        const embi_js_value d = sp[-1];
        for (;;) {
            if (code->bc + code->bc_len - q < 17 || q[0] != EMBI_JSOP_DUP || q[6] != EMBI_JSOP_SEQ || q[7] != EMBI_JSOP_JMP_TRUE ||
                q[12] != EMBI_JSOP_JMP) {
                break;
            }
            embi_js_value k;
            if (q[1] == EMBI_JSOP_PUSH_CONST) {
                k = consts[embi_js_rd32(q + 2)];
            } else if (q[1] == EMBI_JSOP_PUSH_I32) {
                k = embi_js_from_int32((int32_t)embi_js_rd32(q + 2));
            } else {
                break;
            }
            if (seq_values(d, k)) {
                q = code->bc + embi_js_rd32(q + 8);
                break;
            }
            const uint8_t *next = code->bc + embi_js_rd32(q + 13);
            if (next <= q) {
                break;
            }
            q = next;
        }
        pc = q;
        NEXT();
    }
    CASE(CONST_ASSIGN) {
        (void)embi_js_throw_type_error(rt, "Assignment to constant variable.");
        goto throw_;
    }
    CASE(JMP_TRUE) {
        embi_js_value c = *--sp;
        if (TRUTHY(c)) {
            pc = code->bc + OPERAND(1);
        } else {
            pc += 5;
        }
        NEXT();
    }
    CASE(JMP_FALSE) {
        embi_js_value c = *--sp;
        if (!TRUTHY(c)) {
            pc = code->bc + OPERAND(1);
        } else {
            pc += 5;
        }
        NEXT();
    }
    CASE(JMP_TRUE_KEEP) {
        if (TRUTHY(sp[-1])) {
            pc = code->bc + OPERAND(1);
        } else {
            sp--;
            pc += 5;
        }
        NEXT();
    }
    CASE(JMP_FALSE_KEEP) {
        if (!TRUTHY(sp[-1])) {
            pc = code->bc + OPERAND(1);
        } else {
            sp--;
            pc += 5;
        }
        NEXT();
    }
    CASE(ADD) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            int32_t r;
            if (!__builtin_add_overflow(embi_js_as_int32(a), embi_js_as_int32(b), &r)) {
                BIN_RESULT(embi_js_from_int32(r));
            }
        } else if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_number(embi_js_as_double(a) + embi_js_as_double(b)));
        }
        embi_js_value v = op_add(rt, a, b);
        if (v == EXC) {
            goto throw_;
        }
        BIN_RESULT(v);
    }
    CASE(SUB) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            int32_t r;
            if (!__builtin_sub_overflow(embi_js_as_int32(a), embi_js_as_int32(b), &r)) {
                BIN_RESULT(embi_js_from_int32(r));
            }
        } else if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_number(embi_js_as_double(a) - embi_js_as_double(b)));
        }
        ARITH_SLOW(EMBI_JSOP_SUB);
    }
    CASE(MUL) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            int32_t x = embi_js_as_int32(a);
            int32_t y = embi_js_as_int32(b);
            int32_t r;
            if (!__builtin_mul_overflow(x, y, &r)) {
                /* a zero product with a negative operand is -0 */
                if (r == 0 && (x < 0 || y < 0)) {
                    BIN_RESULT(embi_js_from_double(-0.0));
                }
                BIN_RESULT(embi_js_from_int32(r));
            }
        } else if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_number(embi_js_as_double(a) * embi_js_as_double(b)));
        }
        ARITH_SLOW(EMBI_JSOP_MUL);
    }
    CASE(DIV) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            int32_t x = embi_js_as_int32(a);
            int32_t y = embi_js_as_int32(b);
            /* exact quotients only: 0 / -y is -0, and INT32_MIN / -1 leaves the int32 range */
            if (y != 0 && !(x == 0 && y < 0) && !(x == INT32_MIN && y == -1) && x % y == 0) {
                BIN_RESULT(embi_js_from_int32(x / y));
            }
        } else if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_number(embi_js_as_double(a) / embi_js_as_double(b)));
        }
        ARITH_SLOW(EMBI_JSOP_DIV);
    }
    CASE(MOD) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            int32_t x = embi_js_as_int32(a);
            int32_t y = embi_js_as_int32(b);
            if (x >= 0 && y > 0) {
                BIN_RESULT(embi_js_from_int32(x % y));
            }
        } else if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_number(fmod(embi_js_as_double(a), embi_js_as_double(b))));
        }
        ARITH_SLOW(EMBI_JSOP_MOD);
    }
    CASE(EXP) {
        ARITH_SLOW(EMBI_JSOP_EXP);
    }
    CASE(SHL) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            uint32_t s = (uint32_t)embi_js_as_int32(b) & 31u;
            BIN_RESULT(embi_js_from_int32((int32_t)((uint32_t)embi_js_as_int32(a) << s)));
        }
        BITS_SLOW(EMBI_JSOP_SHL);
    }
    CASE(SAR) {
        static_assert((-1 >> 1) == -1, "arithmetic right shift of negative int32");
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            uint32_t s = (uint32_t)embi_js_as_int32(b) & 31u;
            BIN_RESULT(embi_js_from_int32(embi_js_as_int32(a) >> s));
        }
        BITS_SLOW(EMBI_JSOP_SAR);
    }
    CASE(SHR) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            uint32_t s = (uint32_t)embi_js_as_int32(b) & 31u;
            BIN_RESULT(embi_js_number((double)((uint32_t)embi_js_as_int32(a) >> s)));
        }
        BITS_SLOW(EMBI_JSOP_SHR);
    }
    CASE(BAND) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_int32(embi_js_as_int32(a) & embi_js_as_int32(b)));
        }
        BITS_SLOW(EMBI_JSOP_BAND);
    }
    CASE(BOR) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_int32(embi_js_as_int32(a) | embi_js_as_int32(b)));
        }
        BITS_SLOW(EMBI_JSOP_BOR);
    }
    CASE(BXOR) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_int32(embi_js_as_int32(a) ^ embi_js_as_int32(b)));
        }
        BITS_SLOW(EMBI_JSOP_BXOR);
    }
    CASE(EQ) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_bool(a == b));
        }
        if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_double(a) == embi_js_as_double(b)));
        }
        int r = embi_js_loose_equal(rt, a, b);
        if (r == EMBI_JS_THREW) {
            goto throw_;
        }
        BIN_RESULT(embi_js_from_bool(r == EMBI_JS_YES));
    }
    CASE(NE) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_bool(a != b));
        }
        if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_double(a) != embi_js_as_double(b)));
        }
        int r = embi_js_loose_equal(rt, a, b);
        if (r == EMBI_JS_THREW) {
            goto throw_;
        }
        BIN_RESULT(embi_js_from_bool(r == EMBI_JS_NO));
    }
    CASE(SEQ) {
        const bool eq = seq_values(sp[-2], sp[-1]);
        BIN_RESULT(embi_js_from_bool(eq));
    }
    CASE(SNE) {
        const bool eq = seq_values(sp[-2], sp[-1]);
        BIN_RESULT(embi_js_from_bool(!eq));
    }
    CASE(LT) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_int32(a) < embi_js_as_int32(b)));
        }
        if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_double(a) < embi_js_as_double(b)));
        }
        CMP_SLOW(EMBI_JSOP_LT);
    }
    CASE(GT) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_int32(a) > embi_js_as_int32(b)));
        }
        if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_double(a) > embi_js_as_double(b)));
        }
        CMP_SLOW(EMBI_JSOP_GT);
    }
    CASE(LE) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_int32(a) <= embi_js_as_int32(b)));
        }
        if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_double(a) <= embi_js_as_double(b)));
        }
        CMP_SLOW(EMBI_JSOP_LE);
    }
    CASE(GE) {
        embi_js_value a = sp[-2];
        embi_js_value b = sp[-1];
        if (embi_js_is_int32(a) && embi_js_is_int32(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_int32(a) >= embi_js_as_int32(b)));
        }
        if (embi_js_is_number(a) && embi_js_is_number(b)) {
            BIN_RESULT(embi_js_from_bool(embi_js_as_double(a) >= embi_js_as_double(b)));
        }
        CMP_SLOW(EMBI_JSOP_GE);
    }
    CASE(IN) {
        embi_js_value v = op_in(rt, sp[-2], sp[-1]);
        if (v == EXC) {
            goto throw_;
        }
        sp--;
        sp[-1] = v;
        pc += 1;
        NEXT();
    }
    CASE(INSTANCEOF) {
        int r = embi_js_instance_of(rt, sp[-2], sp[-1]);
        if (r == EMBI_JS_THREW) {
            goto throw_;
        }
        sp--;
        sp[-1] = embi_js_from_bool(r == EMBI_JS_YES);
        pc += 1;
        NEXT();
    }
    CASE(NEG) {
        embi_js_value v = sp[-1];
        if (embi_js_is_int32(v) && embi_js_as_int32(v) != 0 && embi_js_as_int32(v) != INT32_MIN) {
            sp[-1] = embi_js_from_int32(-embi_js_as_int32(v));
        } else if (embi_js_is_number(v)) {
            sp[-1] = embi_js_number(-embi_js_as_double(v));
        } else {
            embi_js_value n = embi_js_to_numeric(rt, v);
            if (n == EXC) {
                goto throw_;
            }
            sp[-1] = embi_js_is_bigint(n) ? embi_js_bigint_neg(rt, n)
                                          : embi_js_number(-embi_js_as_double(n));
        }
        pc += 1;
        NEXT();
    }
    CASE(PLUS) {
        double d;
        if (!embi_js_to_number(rt, sp[-1], &d)) {
            goto throw_;
        }
        sp[-1] = embi_js_number(d);
        pc += 1;
        NEXT();
    }
    CASE(NOT) {
        sp[-1] = embi_js_from_bool(!TRUTHY(sp[-1]));
        pc += 1;
        NEXT();
    }
    CASE(BNOT) {
        if (embi_js_is_int32(sp[-1])) {
            sp[-1] = embi_js_from_int32(~embi_js_as_int32(sp[-1]));
            pc += 1;
            NEXT();
        }
        embi_js_value n = embi_js_to_numeric(rt, sp[-1]);
        if (n == EXC) {
            goto throw_;
        }
        if (embi_js_is_bigint(n)) {
            embi_js_value r = embi_js_bigint_not(rt, n);
            if (r == EXC) {
                goto throw_;
            }
            sp[-1] = r;
        } else {
            int32_t x;
            if (!embi_js_to_int32(rt, n, &x)) {
                goto throw_;
            }
            sp[-1] = embi_js_from_int32(~x);
        }
        pc += 1;
        NEXT();
    }
    CASE(TYPEOF) {
        sp[-1] = embi_js_typeof(rt, sp[-1]);
        pc += 1;
        NEXT();
    }
    CASE(TO_NUMERIC) {
        embi_js_value n = embi_js_to_numeric(rt, sp[-1]);
        if (n == EXC) {
            goto throw_;
        }
        sp[-1] = n;
        pc += 1;
        NEXT();
    }
    CASE(INC)
    CASE(DEC) {
        embi_js_value v = sp[-1];
        int32_t delta = pc[0] == EMBI_JSOP_INC ? 1 : -1;
        if (embi_js_is_int32(v)) {
            int32_t r;
            if (!__builtin_add_overflow(embi_js_as_int32(v), delta, &r)) {
                sp[-1] = embi_js_from_int32(r);
                pc += 1;
                NEXT();
            }
        }
        if (embi_js_is_number(v)) {
            sp[-1] = embi_js_number(embi_js_as_double(v) + delta);
            pc += 1;
            NEXT();
        }
        embi_js_value n = embi_js_to_numeric(rt, v);
        if (n == EXC) {
            goto throw_;
        }
        if (embi_js_is_bigint(n)) {
            embi_js_value r = embi_js_bigint_inc(rt, n, delta);
            if (r == EXC) {
                goto throw_;
            }
            sp[-1] = r;
        } else {
            sp[-1] = embi_js_number(embi_js_as_double(n) + delta);
        }
        pc += 1;
        NEXT();
    }
    CASE(PUSH_EMPTY) {
        *sp++ = EMBI_JS_EMPTY;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_NEW_TARGET) {
        *sp++ = FRAME->new_target;
        pc += 1;
        NEXT();
    }
    CASE(TDZ_LOC) {
        if (embi_js_is_empty(slots[OPERAND(1)])) {
            (void)tdz_error(rt, consts[OPERAND(5)]);
            goto throw_;
        }
        pc += 9;
        NEXT();
    }
    CASE(TDZ_BOX) {
        embi_js_box *b = (embi_js_box *)embi_js_as_cell(slots[OPERAND(1)]);
        if (embi_js_is_empty(b->value)) {
            (void)tdz_error(rt, consts[OPERAND(5)]);
            goto throw_;
        }
        pc += 9;
        NEXT();
    }
    CASE(TDZ_UPVAL) {
        embi_js_box *b = (embi_js_box *)embi_js_as_cell(upv[OPERAND(1)]);
        if (embi_js_is_empty(b->value)) {
            (void)tdz_error(rt, consts[OPERAND(5)]);
            goto throw_;
        }
        pc += 9;
        NEXT();
    }
    CASE(TO_STRING) {
        /* A string is its own ToString: keep a rope as it is (J5g). */
        if (!embi_js_is_string(sp[-1])) {
            embi_js_string *s = embi_js_to_string(rt, sp[-1]);
            if (!s) {
                goto throw_;
            }
            sp[-1] = embi_js_from_string(s);
        }
        pc += 1;
        NEXT();
    }
    CASE(OPT_NULLISH) {
        uint32_t ofs = OPERAND(5);
        if (embi_js_is_nullish(sp[-(ptrdiff_t)ofs - 1])) {
            sp -= ofs + 1u;
            *sp++ = UNDEF;
            pc = code->bc + OPERAND(1);
        } else {
            pc += 9;
        }
        NEXT();
    }
    CASE(JMP_NOT_NULLISH_KEEP) {
        if (!embi_js_is_nullish(sp[-1])) {
            pc = code->bc + OPERAND(1);
        } else {
            sp--;
            pc += 5;
        }
        NEXT();
    }
    CASE(ARRAY_FROM_STACK) {
        uint32_t n = OPERAND(1);
        embi_js_object *a = embi_js_array_create_default(rt, 0);
        if (!a) {
            goto throw_;
        }
        embi_js_value *base = sp - n;
        for (uint32_t i = 0; i < n; i++) {
            if (embi_js_obj_create_index_fast(rt, a, i, base[i])) {
                continue;
            }
            if (embi_js_create_data_property(rt, a, embi_js_key_from_index(i), base[i]) ==
                EMBI_JS_THREW) {
                goto throw_;
            }
        }
        sp = base;
        *sp++ = embi_js_from_object(a);
        pc += 5;
        NEXT();
    }
    CASE(ARRAY_PUSH) {
        embi_js_object *a = embi_js_as_object(sp[-2]);
        if (embi_js_array_push_fast(rt, a, sp[-1])) {
            sp--;
            pc += 1;
            NEXT();
        }
        double len = 0;
        if (!embi_js_length_of_array_like(rt, a, &len)) {
            goto throw_;
        }
        if (embi_js_create_data_property(rt, a, embi_js_key_from_index((uint32_t)len), sp[-1]) ==
            EMBI_JS_THREW) {
            goto throw_;
        }
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(ARRAY_HOLE) {
        embi_js_object *a = embi_js_as_object(sp[-1]);
        double len = 0;
        if (!embi_js_length_of_array_like(rt, a, &len)) {
            goto throw_;
        }
        if (embi_js_put(rt, a, embi_js_name_val(rt, EMBI_JS_NAME_LENGTH), embi_js_number(len + 1),
                        true) == EMBI_JS_THREW) {
            goto throw_;
        }
        pc += 1;
        NEXT();
    }
    CASE(ARRAY_SPREAD) {
        if (!array_spread(rt, sp[-2], sp[-1])) {
            goto throw_;
        }
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(CALL_SPREAD) {
        embi_js_value *fp = sp - 3;
        embi_js_value fn = fp[0];
        embi_js_value thisv = fp[1];
        uint32_t argc = 0;
        embi_js_value *argv = nullptr;
        if (!spread_list(rt, fp[2], &argc, &argv)) {
            goto throw_;
        }
        if (interrupted(rt)) {
            spread_free(argv, argc);
            vm->terminating = true;
            (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
            goto throw_;
        }
        FRAME->sp = fp;
        FRAME->pc = pc + 1;
        if (embi_js_is_object(fn) && embi_js_as_object(fn)->klass == &embi_js_class_closure) {
            embi_js_object *fo = embi_js_as_object(fn);
            bool ok = push_frame(rt, fo, code_of(closure_of(fo)->code), thisv, (int)argc, argv,
                                 UNDEF, 0);
            spread_free(argv, argc);
            if (!ok) {
                FRAME->sp = sp;
                goto throw_;
            }
            LOAD_FRAME();
            NEXT();
        }
        if (!embi_js_is_callable(fn)) {
            spread_free(argv, argc);
            FRAME->sp = sp;
            (void)embi_js_throw_type_error(rt, "expression is not a function");
            goto throw_;
        }
        embi_js_value r = embi_js_call(rt, fn, thisv, (int)argc, argv);
        spread_free(argv, argc);
        if (r == EXC) {
            goto throw_;
        }
        sp = fp;
        *sp++ = r;
        pc += 1;
        NEXT();
    }
    CASE(NEW_SPREAD) {
        embi_js_value *fp = sp - 2;
        embi_js_value fn = fp[0];
        uint32_t argc = 0;
        embi_js_value *argv = nullptr;
        if (!spread_list(rt, fp[1], &argc, &argv)) {
            goto throw_;
        }
        if (interrupted(rt)) {
            spread_free(argv, argc);
            vm->terminating = true;
            (void)embi_js_throw_error(rt, EMBI_JS_PROTO_ERROR, "Script was interrupted");
            goto throw_;
        }
        if (embi_js_is_object(fn) && embi_js_as_object(fn)->klass == &embi_js_class_closure &&
            (code_of(closure_of(embi_js_as_object(fn))->code)->flags & EMBI_JS_CODE_CONSTRUCTOR)) {
            embi_js_object *fo = embi_js_as_object(fn);
            embi_js_value proto = embi_js_proto_from_ctor(rt, fn, EMBI_JS_PROTO_OBJECT);
            if (proto == EXC) {
                spread_free(argv, argc);
                goto throw_;
            }
            embi_js_object *self = embi_js_object_new(rt, proto);
            FRAME->sp = fp;
            FRAME->pc = pc + 1;
            bool ok = push_frame(rt, fo, code_of(closure_of(fo)->code), embi_js_from_object(self),
                                 (int)argc, argv, fn, EMBI_JS_FRAME_CONSTRUCT);
            spread_free(argv, argc);
            if (!ok) {
                FRAME->sp = sp;
                goto throw_;
            }
            LOAD_FRAME();
            NEXT();
        }
        FRAME->sp = fp;
        FRAME->pc = pc + 1;
        embi_js_value r = embi_js_construct(rt, fn, (int)argc, argv, fn);
        spread_free(argv, argc);
        if (r == EXC) {
            goto throw_;
        }
        sp = fp;
        *sp++ = r;
        pc += 1;
        NEXT();
    }
    CASE(DEFINE_COMPUTED) {
        embi_js_object *o = embi_js_as_object(sp[-3]);
        embi_js_key k = embi_js_to_property_key(rt, sp[-2]);
        if (k == EXC) {
            goto throw_;
        }
        if (embi_js_create_data_property_or_throw(rt, o, k, sp[-1]) == EMBI_JS_THREW) {
            goto throw_;
        }
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(DEFINE_GETTER_COMPUTED)
    CASE(DEFINE_SETTER_COMPUTED) {
        embi_js_object *o = embi_js_as_object(sp[-3]);
        embi_js_key k = embi_js_to_property_key(rt, sp[-2]);
        if (k == EXC) {
            goto throw_;
        }
        bool getter = pc[0] == EMBI_JSOP_DEFINE_GETTER_COMPUTED;
        embi_js_pdesc d;
        memset(&d, 0, sizeof d);
        d.value = UNDEF;
        d.get = UNDEF;
        d.set = UNDEF;
        d.attrs = ATTR_E | ATTR_C;
        if (getter) {
            d.has = EMBI_JS_PD_GET;
            d.get = sp[-1];
        } else {
            d.has = EMBI_JS_PD_SET;
            d.set = sp[-1];
        }
        d.has |= EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;
        if (embi_js_obj_define_own_property(rt, o, k, &d) == EMBI_JS_THREW) {
            goto throw_;
        }
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(COPY_DATA_PROPS) {
        if (!copy_data_props(rt, embi_js_as_object(sp[-2]), sp[-1])) {
            goto throw_;
        }
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(REQ_COERCIBLE) {
        embi_js_value v = sp[-1];
        if (embi_js_is_nullish(v)) {
            if (embi_js_throw_type_error(rt, "Cannot destructure %s",
                                         v == EMBI_JS_NULL ? "null" : "undefined") == EXC) {
                goto throw_;
            }
        }
        pc += 1;
        NEXT();
    }
    CASE(ITER_INIT) {
        embi_js_value rec = iter_init(rt, sp[-1], true);
        if (rec == EXC) {
            goto throw_;
        }
        sp[-1] = rec;
        pc += 1;
        NEXT();
    }
    CASE(ITER_VALUE) {
        embi_js_value v = UNDEF;
        if (ir_step(rt, ir_of(sp[-1]), &v) < 0) {
            goto throw_;
        }
        sp[-1] = v;
        pc += 1;
        NEXT();
    }
    CASE(ITER_REST) {
        embi_js_value arr = iter_rest(rt, ir_of(sp[-1]));
        if (arr == EXC) {
            goto throw_;
        }
        sp[-1] = arr;
        pc += 1;
        NEXT();
    }
    CASE(GEN_START) {
        /* The body start of a generator: the parameters are bound, and the body has not run. */
        embi_js_object *callee = FRAME->callee;
        FRAME->sp = sp;
        FRAME->pc = pc + 1;
        embi_js_value pv = embi_js_get(rt, callee, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE));
        if (pv == EXC) {
            goto throw_;
        }
        if (!embi_js_is_object(pv)) {
            pv = intrinsic(rt, EMBI_JS_PROTO_GENERATOR);
        }
        embi_js_object *go = embi_js_object_new_class(rt, pv, &embi_js_class_generator);
        embi_js_gen *g = embi_calloc(1, sizeof *g);
        go->internal = g;
        EMBI_JS_WB_STRUCT(go);
        /* Only the live part is kept: the parameters and locals now, and the operand stack when a
         * yield saves it (gen_reserve grows the buffer then). */
        uint32_t live = (uint32_t)(sp - slots);
        g->cap = live ? live : 1u;
        g->saved = embi_calloc(g->cap, sizeof(embi_js_value));
        g->callee = embi_js_from_object(callee);
        g->this_v = FRAME->this_v;
        EMBI_JS_WB(go, g->callee);
        g->state = EMBI_JS_GEN_SUSPENDED_START;
        g->pc = (uint32_t)(pc + 1 - code->bc);
        for (uint32_t i = 0; i < live; i++) {
            EMBI_JS_STORE(go, g->saved[i], slots[i]);
        }
        g->nsaved = live;
        embi_js_value gv = embi_js_from_object(go);
        bool entry = (FRAME->flags & EMBI_JS_FRAME_ENTRY) != 0;
        pop_frame(vm);
        if (entry) {
            return gv;
        }
        LOAD_FRAME();
        *sp++ = gv;
        NEXT();
    }
    CASE(ASYNC_START) {
        /* The body of an async function starts after its parameters: the promise is created, and
         * the frame runs on in place (AWAIT saves it). */
        embi_js_object *holder = embi_js_object_new_class(rt, EMBI_JS_NULL, &embi_js_class_async);
        FRAME->gen = holder;
        embi_js_gen *g = gen_new(0);
        holder->internal = g;
        EMBI_JS_WB_STRUCT(holder);
        g->callee = embi_js_from_object(FRAME->callee);
        g->this_v = FRAME->this_v;
        g->state = EMBI_JS_GEN_EXECUTING;
        EMBI_JS_WB(holder, g->callee);
        EMBI_JS_WB(holder, g->this_v);
        g->promise = embi_js_from_object(embi_js_promise_new(rt));
        EMBI_JS_WB(holder, g->promise);
        pc += 1;
        NEXT();
    }
    CASE(ASYNC_GEN_START) {
        /* The body start of an async generator: GEN_START with the async generator prototype. */
        embi_js_object *callee = FRAME->callee;
        FRAME->sp = sp;
        FRAME->pc = pc + 1;
        embi_js_value pv = embi_js_get(rt, callee, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE));
        if (pv == EXC) {
            goto throw_;
        }
        if (!embi_js_is_object(pv)) {
            pv = intrinsic(rt, EMBI_JS_PROTO_ASYNC_GENERATOR);
        }
        embi_js_object *go = embi_js_object_new_class(rt, pv, &embi_js_class_async_gen);
        uint32_t live = (uint32_t)(sp - slots);
        embi_js_gen *g = gen_new(live);
        go->internal = g;
        EMBI_JS_WB_STRUCT(go);
        g->callee = embi_js_from_object(callee);
        g->this_v = FRAME->this_v;
        EMBI_JS_WB(go, g->callee);
        EMBI_JS_WB(go, g->this_v);
        g->state = EMBI_JS_GEN_SUSPENDED_START;
        g->pc = (uint32_t)(pc + 1 - code->bc);
        for (uint32_t i = 0; i < live; i++) {
            EMBI_JS_STORE(go, g->saved[i], slots[i]);
        }
        g->nsaved = live;
        embi_js_value gv = embi_js_from_object(go);
        bool entry = (FRAME->flags & EMBI_JS_FRAME_ENTRY) != 0;
        pop_frame(vm);
        if (entry) {
            return gv;
        }
        LOAD_FRAME();
        *sp++ = gv;
        NEXT();
    }
    CASE(AWAIT) {
        /* [v] -> [received mode]: the frame suspends until v settles (GEN_RESUME 0 follows). */
        embi_js_value v = sp[-1];
        embi_js_object *hold = FRAME->gen;
        FRAME->sp = sp;
        FRAME->pc = pc;
        embi_js_value pv = embi_js_promise_resolve(rt, intrinsic(rt, EMBI_JS_PROTO_PROMISE_CTOR), v);
        if (pv == EXC) {
            goto throw_;
        }
        embi_js_value on_ok = await_fn_new(rt, hold, AK_FN_OK);
        embi_js_value on_err = await_fn_new(rt, hold, AK_FN_ERR);
        (void)embi_js_promise_then(rt, embi_js_as_object(pv), on_ok, on_err, nullptr);
        embi_js_value result = hold->klass == &embi_js_class_async ? gen_of(hold)->promise
                                                                  : EMBI_JS_UNDEFINED;
        bool entry = (FRAME->flags & EMBI_JS_FRAME_ENTRY) != 0;
        gen_save(vm, slots, sp - 1, pc + 1, EMBI_JS_SUSP_AWAIT);
        pop_frame(vm);
        if (entry) {
            return result;
        }
        LOAD_FRAME();
        *sp++ = result;
        NEXT();
    }
    CASE(ASYNC_YIELD) {
        /* [v] -> [received mode]: the head request gets {value: v, done: false}. The queue's next
         * request, when there is one, resumes the body at once; otherwise the body suspends. */
        embi_js_value v = sp[-1];
        embi_js_object *hold = FRAME->gen;
        FRAME->sp = sp;
        FRAME->pc = pc;
        agen_complete(rt, hold, false, v, false);
        embi_js_gen *g = gen_of(hold);
        if (g->qhead != EMBI_JS_EMPTY) {
            embi_js_varray *rq = vr_of(g->qhead);
            sp[-1] = rq->elems[0];
            *sp++ = rq->elems[1];
            pc += 1;
            NEXT();
        }
        bool entry = (FRAME->flags & EMBI_JS_FRAME_ENTRY) != 0;
        gen_suspend(vm, slots, sp - 1, pc + 1, false);
        pop_frame(vm);
        if (entry) {
            return EMBI_JS_UNDEFINED;
        }
        LOAD_FRAME();
        *sp++ = EMBI_JS_UNDEFINED;
        NEXT();
    }
    CASE(ASYNC_ITER_INIT) {
        FRAME->sp = sp;
        FRAME->pc = pc;
        embi_js_value rec = async_iter_init(rt, sp[-1]);
        if (rec == EXC) {
            goto throw_;
        }
        sp[-1] = rec;
        pc += 1;
        NEXT();
    }
    CASE(ASYNC_ITER_CALL) {
        /* [rec] -> [rec res]: the next call; its result is awaited by the AWAIT that follows. */
        embi_js_varray *rec = ir_of(sp[-1]);
        FRAME->sp = sp;
        FRAME->pc = pc;
        embi_js_value res = embi_js_call(rt, rec->elems[1], rec->elems[0], 0, nullptr);
        if (res == EXC) {
            goto throw_;
        }
        *sp++ = res;
        pc += 1;
        NEXT();
    }
    CASE(ASYNC_ITER_STEP) {
        /* [rec received] -> [rec v], or the done jump with [rec]. */
        embi_js_value recv = sp[-1];
        if (!embi_js_is_object(recv)) {
            (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
            goto throw_;
        }
        FRAME->sp = sp;
        FRAME->pc = pc;
        embi_js_value d = embi_js_get(rt, embi_js_as_object(recv), iter_keys(rt)->k_done);
        if (d == EXC) {
            goto throw_;
        }
        if (embi_js_to_boolean(d)) {
            sp--;
            pc = code->bc + OPERAND(1);
            NEXT();
        }
        embi_js_value v = embi_js_get(rt, embi_js_as_object(recv), iter_keys(rt)->k_value);
        if (v == EXC) {
            goto throw_;
        }
        sp[-1] = v;
        pc += 5;
        NEXT();
    }
    CASE(ASYNC_CLOSE_CALL) {
        /* [rec] -> [rec res]: the return method's call; without one, the jump with [rec]. The
         * record is marked done before the call, as ir_close does, so an exception that the
         * return call raises (and the loop's handler, which covers this op) does not close the
         * iterator a second time. A record that is already done is not closed again. */
        embi_js_varray *rec = ir_of(sp[-1]);
        FRAME->sp = sp;
        FRAME->pc = pc;
        if (ir_flags(rec) & IR_DONE) {
            pc = code->bc + OPERAND(1);
            NEXT();
        }
        ir_set_flags(rec, ir_flags(rec) | IR_DONE);
        embi_js_value iter = rec->elems[0];
        embi_js_value rf = embi_js_get_method(rt, iter, iter_keys(rt)->k_return);
        if (rf == EXC) {
            goto throw_;
        }
        if (rf == UNDEF) {
            pc = code->bc + OPERAND(1);
            NEXT();
        }
        embi_js_value res = embi_js_call(rt, rf, iter, 0, nullptr);
        if (res == EXC) {
            goto throw_;
        }
        *sp++ = res;
        pc += 5;
        NEXT();
    }
    CASE(ASYNC_CLOSE_CHECK) {
        /* [rec received] -> [rec]: the awaited return result must be an object. */
        if (!embi_js_is_object(sp[-1])) {
            (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
            goto throw_;
        }
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(ASYNC_DELEGATE_CALL) {
        /* [rec received mode] -> [rec mode res]: the inner next (mode 0), throw (1) or return (2),
         * called with received. A missing throw jumps to operand 1 with [rec]; a missing return
         * jumps to operand 2 with [received]. */
        embi_js_varray *rec = ir_of(sp[-3]);
        embi_js_value recv = sp[-2];
        int32_t mode = embi_js_as_int32(sp[-1]);
        embi_js_value iter = rec->elems[0];
        embi_js_value res = UNDEF;
        FRAME->sp = sp;
        FRAME->pc = pc;
        if (mode == 0) {
            res = embi_js_call(rt, rec->elems[1], iter, 1, &recv);
        } else if (mode == 1) {
            embi_js_value th = embi_js_get_method(rt, iter, iter_keys(rt)->k_throw);
            if (th == EXC) {
                goto throw_;
            }
            if (th == UNDEF) {
                sp -= 2;
                pc = code->bc + OPERAND(1);
                NEXT();
            }
            res = embi_js_call(rt, th, iter, 1, &recv);
        } else {
            embi_js_value rf = embi_js_get_method(rt, iter, iter_keys(rt)->k_return);
            if (rf == EXC) {
                goto throw_;
            }
            if (rf == UNDEF) {
                sp[-3] = recv;
                sp -= 2;
                pc = code->bc + OPERAND(5);
                NEXT();
            }
            res = embi_js_call(rt, rf, iter, 1, &recv);
        }
        if (res == EXC) {
            goto throw_;
        }
        sp[-2] = sp[-1];
        sp[-1] = res;
        pc += 9;
        NEXT();
    }
    CASE(ASYNC_DELEGATE_CHECK) {
        /* [rec mode received awaitMode] -> [rec received] (not done) or, when done, [value] with a
         * jump to operand 1 (a return completion, when mode is 2) or operand 2. */
        embi_js_value received = sp[-2];
        FRAME->sp = sp;
        FRAME->pc = pc;
        if (embi_js_as_int32(sp[-1]) == 1) {
            embi_js_exception_set(rt, received);
            sp -= 4;
            goto throw_;
        }
        if (!embi_js_is_object(received)) {
            (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
            goto throw_;
        }
        embi_js_value d = embi_js_get(rt, embi_js_as_object(received), iter_keys(rt)->k_done);
        if (d == EXC) {
            goto throw_;
        }
        embi_js_value v = embi_js_get(rt, embi_js_as_object(received), iter_keys(rt)->k_value);
        if (v == EXC) {
            goto throw_;
        }
        int32_t mode = embi_js_as_int32(sp[-3]);
        if (embi_js_to_boolean(d)) {
            sp[-4] = v;
            sp -= 3;
            pc = code->bc + OPERAND(mode == 2 ? 1 : 5);
            NEXT();
        }
        sp[-3] = v;
        sp -= 2;
        pc += 9;
        NEXT();
    }
    CASE(ASYNC_DELEGATE_NOTHROW) {
        /* [rec] -> throws the TypeError of a missing throw method (the iterator is closed by then). */
        FRAME->sp = sp;
        FRAME->pc = pc;
        (void)embi_js_throw_type_error(rt, "The iterator does not provide a 'throw' method");
        goto throw_;
    }
    CASE(ASYNC_RET_PREP) {
        /* [rec received mode]: a return resumption moves to [rec mode received], so its value is
         * awaited next; other modes continue at operand 1 unchanged. */
        if (embi_js_as_int32(sp[-1]) != 2) {
            pc = code->bc + OPERAND(1);
            NEXT();
        }
        embi_js_value recv = sp[-2];
        sp[-2] = sp[-1];
        sp[-1] = recv;
        pc += 5;
        NEXT();
    }
    CASE(ASYNC_RET_FINISH) {
        /* [rec mode received awaitMode] -> [rec received mode']: a fulfilled return value resumes
         * as a return (2), a rejected one as a throw (1). */
        embi_js_value received = sp[-2];
        int32_t await_mode = embi_js_as_int32(sp[-1]);
        sp[-3] = received;
        sp[-2] = embi_js_from_int32(await_mode == 0 ? 2 : 1);
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(YIELD) {
        embi_js_value v = sp[-1];
        gen_suspend(vm, slots, sp - 1, pc + 1, false);
        pop_frame(vm);
        return v;
    }
    CASE(YIELD_RAW) {
        embi_js_value v = sp[-1];
        gen_suspend(vm, slots, sp - 1, pc + 1, true);
        pop_frame(vm);
        return v;
    }
    CASE(GEN_RESUME) {
        /* [received mode]: throw at this op, return by jumping to the return path, or next. */
        int32_t mode = embi_js_as_int32(sp[-1]);
        if (mode == 1) {
            embi_js_exception_set(rt, sp[-2]);
            sp -= 2;
            goto throw_;
        }
        sp--;
        if (mode == 2) {
            pc = code->bc + OPERAND(1);
            NEXT();
        }
        pc += 5;
        NEXT();
    }
    CASE(DELEGATE_STEP) {
        /* yield*: [rec received mode] forwards the mode to the inner iterator. */
        embi_js_varray *rec = ir_of(sp[-3]);
        embi_js_value recv = sp[-2];
        int32_t mode = embi_js_as_int32(sp[-1]);
        embi_js_value iter = rec->elems[0];
        embi_js_value res = UNDEF;
        FRAME->sp = sp;
        FRAME->pc = pc;
        if (mode == 0) {
            res = embi_js_call(rt, rec->elems[1], iter, 1, &recv);
        } else if (mode == 1) {
            embi_js_value th = embi_js_get_method(rt, iter, iter_keys(rt)->k_throw);
            if (th == EXC) {
                goto throw_;
            }
            if (th == UNDEF) {
                if (!ir_close(rt, rec)) {
                    goto throw_;
                }
                (void)embi_js_throw_type_error(rt, "The iterator does not provide a 'throw' method");
                goto throw_;
            }
            res = embi_js_call(rt, th, iter, 1, &recv);
        } else {
            embi_js_value rf = embi_js_get_method(rt, iter, iter_keys(rt)->k_return);
            if (rf == EXC) {
                goto throw_;
            }
            if (rf == UNDEF) {
                sp[-3] = recv;
                sp -= 2;
                pc = code->bc + OPERAND(5);
                NEXT();
            }
            res = embi_js_call(rt, rf, iter, 1, &recv);
        }
        if (res == EXC) {
            goto throw_;
        }
        if (!embi_js_is_object(res)) {
            (void)embi_js_throw_type_error(rt, "Iterator result is not an object");
            goto throw_;
        }
        embi_js_value d = embi_js_get(rt, embi_js_as_object(res), iter_keys(rt)->k_done);
        if (d == EXC) {
            goto throw_;
        }
        if (embi_js_to_boolean(d)) {
            embi_js_value v = embi_js_get(rt, embi_js_as_object(res), iter_keys(rt)->k_value);
            if (v == EXC) {
                goto throw_;
            }
            sp[-3] = v;
            sp -= 2;
            pc = code->bc + OPERAND(mode == 2 ? 5 : 1);
            NEXT();
        }
        /* Not done: [rec result] is yielded by the YIELD_RAW that follows. */
        sp[-2] = res;
        sp--;
        pc += 9;
        NEXT();
    }
    CASE(ITER_GET) {
        embi_js_value rec = iter_init(rt, sp[-1], false);
        if (rec == EXC) {
            goto throw_;
        }
        sp[-1] = rec;
        pc += 1;
        NEXT();
    }
    CASE(ITER_NEXT) {
        embi_js_value v = UNDEF;
        int st = ir_step(rt, ir_of(sp[-1]), &v);
        if (st < 0) {
            goto throw_;
        }
        if (st == 0) {
            pc = code->bc + OPERAND(1);
            NEXT();
        }
        *sp++ = v;
        pc += 5;
        NEXT();
    }
    CASE(ITER_CLOSE_KEEP) {
        if (!ir_close(rt, ir_of(sp[-1]))) {
            goto throw_;
        }
        pc += 1;
        NEXT();
    }
    CASE(ITER_CLOSE) {
        if (!ir_close(rt, ir_of(sp[-1]))) {
            goto throw_;
        }
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(ITER_CLOSE_THROW) {
        ir_close_throw(rt, ir_of(sp[-1]));
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(OBJ_REST) {
        embi_js_object *o = embi_js_object_new_plain(rt);
        if (!copy_rest(rt, o, sp[-2], sp[-1])) {
            goto throw_;
        }
        sp[-2] = embi_js_from_object(o);
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(SET_PROTO_LIT) {
        embi_js_value v = sp[-1];
        if (embi_js_is_object(v) || v == EMBI_JS_NULL) {
            if (embi_js_obj_set_prototype_of(rt, embi_js_as_object(sp[-2]), v) == EMBI_JS_THREW) {
                goto throw_;
            }
        }
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(SET_FN_NAME) {
        embi_js_string *name = key_fn_name(rt, sp[-2], OPERAND(1));
        if (!name) {
            goto throw_;
        }
        embi_js_define_raw(rt, embi_js_as_object(sp[-1]), embi_js_name_val(rt, EMBI_JS_NAME_NAME),
                           embi_js_from_string(name), ATTR_C);
        pc += 5;
        NEXT();
    }
    CASE(ROT5) {
        embi_js_value e = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = sp[-3];
        sp[-3] = sp[-4];
        sp[-4] = sp[-5];
        sp[-5] = e;
        pc += 1;
        NEXT();
    }
    CASE(DUP3) {
        sp[0] = sp[-3];
        sp[1] = sp[-2];
        sp[2] = sp[-1];
        sp += 3;
        pc += 1;
        NEXT();
    }
    CASE(PUSH_HOME) {
        embi_js_closure *cl = FRAME->callee ? closure_or_null(embi_js_from_object(FRAME->callee))
                                            : nullptr;
        *sp++ = cl ? cl_home(cl) : UNDEF;
        pc += 1;
        NEXT();
    }
    CASE(SET_HOME) {
        embi_js_value home = sp[-2 - (ptrdiff_t)OPERAND(1)];
        embi_js_closure *cl = closure_or_null(sp[-1]);
        if (cl) {
            cl_set_home(embi_js_as_object(sp[-1]), home);
        }
        pc += 5;
        NEXT();
    }
    CASE(SUPER_GET) {
        embi_js_value thisv = sp[-2];
        embi_js_value proto = super_proto(rt, sp[-1]);
        if (proto == EXC) {
            goto throw_;
        }
        embi_js_value v = embi_js_ic_get(rt, (embi_js_cell *)code, &code->ics[OPERAND(1)],
                                         embi_js_as_object(proto), thisv);
        if (v == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp--;
        pc += 5;
        NEXT();
    }
    CASE(SUPER_BASE) {
        embi_js_value base = super_base_new(rt, sp[-2]);
        if (base == EXC) {
            goto throw_;
        }
        sp[-2] = base;
        pc += 1;
        NEXT();
    }
    CASE(SUPER_GET_INDEX) {
        /* GetSuperBase comes before ToPropertyKey. */
        embi_js_value thisv = sp[-3];
        embi_js_value proto = super_proto(rt, sp[-2]);
        if (proto == EXC) {
            goto throw_;
        }
        embi_js_key k = embi_js_to_property_key(rt, sp[-1]);
        if (k == EXC) {
            goto throw_;
        }
        embi_js_value v = embi_js_obj_get(rt, embi_js_as_object(proto), k, thisv);
        if (v == EXC) {
            goto throw_;
        }
        sp[-3] = v;
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(SUPER_SET) {
        embi_js_value thisv = sp[-3];
        embi_js_value v = sp[-1];
        embi_js_value proto = super_proto(rt, sp[-2]);
        if (proto == EXC) {
            goto throw_;
        }
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        int r = embi_js_obj_set(rt, embi_js_as_object(proto), k, v, thisv);
        if (r == EMBI_JS_THREW) {
            goto throw_;
        }
        if (r == EMBI_JS_NO && (code->flags & EMBI_JS_CODE_STRICT)) {
            (void)embi_js_throw_type_error(rt, "Cannot assign to read only property of super");
            goto throw_;
        }
        sp[-3] = v;
        sp -= 2;
        pc += 5;
        NEXT();
    }
    CASE(SUPER_SET_INDEX) {
        embi_js_value thisv = sp[-4];
        embi_js_value v = sp[-1];
        embi_js_value proto = super_proto(rt, sp[-3]);
        if (proto == EXC) {
            goto throw_;
        }
        embi_js_key k = embi_js_to_property_key(rt, sp[-2]);
        if (k == EXC) {
            goto throw_;
        }
        int r = embi_js_obj_set(rt, embi_js_as_object(proto), k, v, thisv);
        if (r == EMBI_JS_THREW) {
            goto throw_;
        }
        if (r == EMBI_JS_NO && (code->flags & EMBI_JS_CODE_STRICT)) {
            (void)embi_js_throw_type_error(rt, "Cannot assign to read only property of super");
            goto throw_;
        }
        sp[-4] = v;
        sp -= 3;
        pc += 1;
        NEXT();
    }
    CASE(SUPER_GET_THIS) {
        embi_js_value thisv = sp[-2];
        embi_js_value proto = super_proto(rt, sp[-1]);
        if (proto == EXC) {
            goto throw_;
        }
        embi_js_key k = embi_js_key_from_str(rt, embi_js_as_string(consts[OPERAND(1)]));
        embi_js_value v = embi_js_obj_get(rt, embi_js_as_object(proto), k, thisv);
        if (v == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp[-1] = thisv;
        pc += 5;
        NEXT();
    }
    CASE(SUPER_GET_THIS_INDEX) {
        embi_js_value thisv = sp[-3];
        embi_js_value proto = super_proto(rt, sp[-2]);
        if (proto == EXC) {
            goto throw_;
        }
        embi_js_key k = embi_js_to_property_key(rt, sp[-1]);
        if (k == EXC) {
            goto throw_;
        }
        embi_js_value v = embi_js_obj_get(rt, embi_js_as_object(proto), k, thisv);
        if (v == EXC) {
            goto throw_;
        }
        sp[-3] = v;
        sp[-2] = thisv;
        sp -= 1;
        pc += 1;
        NEXT();
    }
    CASE(SUPER_CALL) {
        uint32_t argc = OPERAND(1);
        embi_js_value *fp = sp - argc - 2;
        embi_js_value parent = UNDEF;
        if (embi_js_obj_get_prototype_of(rt, embi_js_as_object(fp[0]), &parent) == EMBI_JS_THREW) {
            goto throw_;
        }
        if (!embi_js_is_constructor(parent)) {
            (void)embi_js_throw_type_error(rt, "Super constructor is not a constructor");
            goto throw_;
        }
        embi_js_value r = embi_js_construct(rt, parent, (int)argc, fp + 2, fp[1]);
        if (r == EXC) {
            goto throw_;
        }
        sp = fp;
        *sp++ = r;
        pc += 5;
        NEXT();
    }
    CASE(SUPER_CALL_SPREAD) {
        embi_js_value *fp = sp - 3;
        embi_js_value parent = UNDEF;
        if (embi_js_obj_get_prototype_of(rt, embi_js_as_object(fp[0]), &parent) == EMBI_JS_THREW) {
            goto throw_;
        }
        if (!embi_js_is_constructor(parent)) {
            (void)embi_js_throw_type_error(rt, "Super constructor is not a constructor");
            goto throw_;
        }
        uint32_t n = 0;
        embi_js_value *buf = nullptr;
        if (!spread_list(rt, fp[2], &n, &buf)) {
            goto throw_;
        }
        embi_js_value r = embi_js_construct(rt, parent, (int)n, buf, fp[1]);
        spread_free(buf, n);
        if (r == EXC) {
            goto throw_;
        }
        fp[0] = r;
        sp = fp + 1;
        pc += 1;
        NEXT();
    }
    CASE(SUPER_DELETE) {
        sp -= 2;
        (void)embi_js_throw_reference_error(rt, "Unsupported reference to 'super'");
        goto throw_;
    }
    CASE(CHECK_THIS) {
        if (sp[-1] == EMBI_JS_EMPTY) {
            (void)embi_js_throw_reference_error(rt, "Must call super constructor before using 'this'");
            goto throw_;
        }
        pc += 1;
        NEXT();
    }
    CASE(THIS_UNINIT) {
        if (sp[-1] != EMBI_JS_EMPTY) {
            (void)embi_js_throw_reference_error(rt, "Super constructor may only be called once");
            goto throw_;
        }
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(CTOR_RETURN) {
        embi_js_value v = sp[-2];
        embi_js_value thisv = sp[-1];
        embi_js_value r = v;
        if (!embi_js_is_object(v)) {
            if (v != UNDEF) {
                (void)embi_js_throw_type_error(rt, "Derived constructors may only return object or undefined");
                goto throw_;
            }
            if (thisv == EMBI_JS_EMPTY) {
                (void)embi_js_throw_reference_error(rt, "Must call super constructor before returning");
                goto throw_;
            }
            r = thisv;
        }
        sp[-2] = r;
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(INIT_FIELDS) {
        if (!class_fields_init(rt, sp[-2], sp[-1])) {
            goto throw_;
        }
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(CLASS_SETUP) {
        embi_js_value heritage = sp[-2];
        embi_js_object *F = embi_js_as_object(sp[-1]);
        embi_js_value proto_parent = intrinsic(rt, EMBI_JS_PROTO_OBJECT);
        embi_js_value ctor_parent = intrinsic(rt, EMBI_JS_PROTO_FUNCTION);
        if (heritage != EMBI_JS_EMPTY) {
            if (heritage == EMBI_JS_NULL) {
                proto_parent = EMBI_JS_NULL;
            } else {
                if (!embi_js_is_constructor(heritage)) {
                    (void)embi_js_throw_type_error(rt, "Class extends value is not a constructor or null");
                    goto throw_;
                }
                embi_js_value pp = embi_js_obj_get(rt, embi_js_as_object(heritage),
                                                   embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                                                   heritage);
                if (pp == EXC) {
                    goto throw_;
                }
                if (!embi_js_is_object(pp) && pp != EMBI_JS_NULL) {
                    (void)embi_js_throw_type_error(rt, "Class extends value does not have valid prototype property");
                    goto throw_;
                }
                proto_parent = pp;
                ctor_parent = heritage;
            }
        }
        embi_js_object *P = embi_js_object_new(rt, proto_parent);
        embi_js_define_raw(rt, F, embi_js_name_val(rt, EMBI_JS_NAME_PROTOTYPE),
                           embi_js_from_object(P), 0);
        embi_js_define_raw(rt, P, embi_js_name_val(rt, EMBI_JS_NAME_CONSTRUCTOR),
                           embi_js_from_object(F), ATTR_W | ATTR_C);
        if (embi_js_obj_set_prototype_of(rt, F, ctor_parent) != EMBI_JS_YES) {
            goto throw_;
        }
        cl_set_home(F, embi_js_from_object(P));
        sp[-2] = embi_js_from_object(F);
        sp[-1] = embi_js_from_object(P);
        pc += 1;
        NEXT();
    }
    CASE(CLASS_SET_INIT) {
        embi_js_closure *cl = closure_or_null(sp[-4]);
        if (cl) {
            /* Both are barriered on the constructor: a class without fields has an undefined
             * finit, and the owner must still be remembered for a young pinit. */
            embi_js_object *ctor = embi_js_as_object(sp[-4]);
            embi_js_closure_ext *e = cl_ext_ensure(ctor);
            EMBI_JS_STORE(ctor, e->pinit, sp[-2]);
            EMBI_JS_STORE(ctor, e->finit, sp[-1]);
        }
        sp -= 4;
        pc += 1;
        NEXT();
    }
    CASE(CLASS_RUN_STATIC) {
        embi_js_value F = sp[-2];
        embi_js_value sinit = sp[-1];
        embi_js_closure *cl = closure_or_null(sinit);
        if (cl) {
            cl_set_home(embi_js_as_object(sinit), F);
        }
        if (embi_js_call(rt, sinit, F, 0, nullptr) == EXC) {
            goto throw_;
        }
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(NEW_PRIVATE) {
        embi_js_symbol *s = embi_js_symbol_new(rt, consts[OPERAND(1)]);
        embi_js_value sv = embi_js_from_symbol(s);
        embi_js_as_cell(sv)->flags |= EMBI_JS_FLAG_SYM_PRIVATE;
        *sp++ = sv;
        pc += 5;
        NEXT();
    }
    CASE(PRIV_GET) {
        embi_js_value v = priv_get(rt, sp[-2], sp[-1]);
        if (v == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(PRIV_GET_THIS) {
        embi_js_value obj = sp[-2];
        embi_js_value v = priv_get(rt, obj, sp[-1]);
        if (v == EXC) {
            goto throw_;
        }
        sp[-2] = v;
        sp[-1] = obj;
        pc += 1;
        NEXT();
    }
    CASE(PRIV_SET) {
        if (priv_write(rt, sp[-3], sp[-2], sp[-1]) == EXC) {
            goto throw_;
        }
        sp[-3] = sp[-1];
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(PRIV_IN) {
        if (!embi_js_is_object(sp[-1])) {
            (void)throw_name(rt, EMBI_JS_PROTO_TYPE_ERROR, "Cannot use 'in' operator to search for '",
                             priv_text(sp[-2]), "' in a non-object");
            goto throw_;
        }
        embi_js_pdesc d;
        memset(&d, 0, sizeof d);
        sp[-2] = embi_js_from_bool(priv_find(sp[-1], sp[-2], &d));
        sp--;
        pc += 1;
        NEXT();
    }
    CASE(PRIV_DEF) {
        if (priv_add_check(rt, sp[-3], sp[-2]) == EXC) {
            goto throw_;
        }
        embi_js_define_raw(rt, embi_js_as_object(sp[-3]), sp[-2], sp[-1], ATTR_W);
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(PRIV_DEF_METHOD) {
        if (priv_add_check(rt, sp[-3], sp[-2]) == EXC) {
            goto throw_;
        }
        embi_js_define_raw(rt, embi_js_as_object(sp[-3]), sp[-2], sp[-1], 0);
        sp -= 2;
        pc += 1;
        NEXT();
    }
    CASE(PRIV_DEF_ACCESSOR) {
        if (priv_add_check(rt, sp[-4], sp[-3]) == EXC) {
            goto throw_;
        }
        embi_js_define_accessor_raw(rt, embi_js_as_object(sp[-4]), sp[-3], sp[-2], sp[-1], 0);
        sp -= 3;
        pc += 1;
        NEXT();
    }
    CASE(DEFINE_METHOD) {
        uint32_t kind = OPERAND(1);
        embi_js_object *o = embi_js_as_object(sp[-3]);
        embi_js_value fnv = sp[-1];
        embi_js_key k = embi_js_to_property_key(rt, sp[-2]);
        if (k == EXC) {
            goto throw_;
        }
        embi_js_closure *cl = closure_or_null(fnv);
        if (cl) {
            cl_set_home(embi_js_as_object(fnv), sp[-3]);
        }
        if (set_method_name(rt, fnv, k, kind) == EXC) {
            goto throw_;
        }
        embi_js_pdesc d;
        memset(&d, 0, sizeof d);
        d.value = UNDEF;
        d.get = UNDEF;
        d.set = UNDEF;
        if (kind == 0) {
            d.has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE |
                    EMBI_JS_PD_CONFIGURABLE;
            d.value = fnv;
            d.attrs = ATTR_W | ATTR_C;
        } else {
            d.has = (kind == 1 ? EMBI_JS_PD_GET : EMBI_JS_PD_SET) | EMBI_JS_PD_ENUMERABLE |
                    EMBI_JS_PD_CONFIGURABLE;
            if (kind == 1) {
                d.get = fnv;
            } else {
                d.set = fnv;
            }
            d.attrs = ATTR_C;
        }
        if (embi_js_define_property_or_throw(rt, o, k, &d) == EMBI_JS_THREW) {
            goto throw_;
        }
        sp -= 3;
        pc += 5;
        NEXT();
    }
    CASE(FOR_IN_INIT) {
        embi_js_value it = for_in_init(rt, sp[-1]);
        if (it == EXC) {
            goto throw_;
        }
        sp[-1] = it;
        pc += 1;
        NEXT();
    }
    CASE(FOR_IN_NEXT) {
        embi_js_varray *it = (embi_js_varray *)embi_js_as_cell(sp[-1]);
        uint32_t len = it->hdr.aux;
        bool found = false;
        embi_js_value key = UNDEF;
        while (embi_js_is_int32(it->elems[1]) && (uint32_t)embi_js_as_int32(it->elems[1]) + 2u < len) {
            uint32_t i = (uint32_t)embi_js_as_int32(it->elems[1]);
            EMBI_JS_STORE(it, it->elems[1], embi_js_from_int32((int32_t)(i + 1u)));
            EMBI_JS_WB(it, it->elems[1]);
            embi_js_value cand = it->elems[i + 2u];
            if (!embi_js_is_object(it->elems[0])) {
                continue;
            }
            embi_js_key k = embi_js_to_property_key(rt, cand);
            if (k == EXC) {
                goto throw_;
            }
            /* A proxy's has trap cannot see keys that came from a trap-supplied prototype. */
            int has = embi_js_obj_is_proxy(embi_js_as_object(it->elems[0]))
                          ? EMBI_JS_YES
                          : embi_js_obj_has_property(rt, embi_js_as_object(it->elems[0]), k);
            if (has == EMBI_JS_THREW) {
                goto throw_;
            }
            if (has == EMBI_JS_YES) {
                key = cand;
                found = true;
                break;
            }
        }
        if (found) {
            *sp++ = key;
            pc += 5;
        } else {
            pc = code->bc + OPERAND(1);
        }
        NEXT();
    }

    /* ---- J5i superinstructions (opcodes.def). Each handler performs the work of its two parts in
     * their order, leaves the stack the pair would leave, and advances by the pair's size. The
     * operands are the parts' own, at their unfused offsets. */
    CASE(GET_LOC_I32) {
        *sp++ = slots[OPERAND(1)];
        *sp++ = embi_js_from_int32((int32_t)OPERAND(6));
        pc += 10;
        NEXT();
    }
    CASE(INC_SET_LOC) {
        embi_js_value v = sp[-1];
        embi_js_value n;
        int32_t r;
        if (embi_js_is_int32(v) && !__builtin_add_overflow(embi_js_as_int32(v), 1, &r)) {
            n = embi_js_from_int32(r);
        } else if (embi_js_is_number(v)) {
            n = embi_js_number(embi_js_as_double(v) + 1);
        } else {
            n = op_step_slow(rt, v, 1);
            if (n == EXC) {
                goto throw_;
            }
        }
        sp[-1] = n;
        slots[OPERAND(2)] = n;
        pc += 6;
        NEXT();
    }

#if !EMBI_JS_COMPUTED_GOTO
    default:
        EMBI_UNREACHABLE();
    }
#endif

throw_:
    {
        embi_js_value exc = rt->pending_exception;
        uint32_t top_off = (uint32_t)(pc - code->bc);
        if (!rethrown) {
            record_throw(rt, vm, exc, top_off);
        }
        rethrown = false;
        for (;;) {
            if (!vm->terminating) {
                for (uint32_t i = 0; i < code->nexc; i++) {
                    const embi_js_exc_entry *e = &code->exc[i];
                    uint32_t off = (uint32_t)(pc - code->bc);
                    if (off >= e->start && off < e->end) {
                        embi_js_value *top = slots + code->nparams + code->nlocals + e->depth;
                        top[0] = exc;
                        sp = top + 1;
                        embi_js_exception_clear(rt);
                        /* A catch ends the exception: the next throw records its own location.
                         * A finally entry keeps the record, since the exception continues. */
                        if (!e->finally_entry) {
                            vm->loc_valid = false;
                        } else {
                            save_location(vm);
                        }
                        pc = code->bc + e->handler;
                        goto resume_handler;
                    }
                }
            }
            if (!vm->terminating && (FRAME->flags & EMBI_JS_FRAME_ASYNC)) {
                /* An async function: the throw rejects its promise, and the frame returns it. */
                embi_js_value res = async_settle(rt, FRAME->gen, exc, true);
                embi_js_exception_clear(rt);
                vm->loc_valid = false;
                bool entry_p = (FRAME->flags & EMBI_JS_FRAME_ENTRY) != 0;
                pop_frame(vm);
                if (entry_p) {
                    return res;
                }
                LOAD_FRAME();
                *sp++ = res;
                NEXT();
            }
            bool entry = (FRAME->flags & EMBI_JS_FRAME_ENTRY) != 0;
            pop_frame(vm);
            if (entry) {
                return EXC;
            }
            LOAD_FRAME();
            pc = FRAME->pc - 1;
        }
    }
resume_handler:
    NEXT();

#undef CASE
#undef NEXT
#undef OPERAND
#undef LOAD_FRAME
#undef TRUTHY
#undef BIN_RESULT
#undef ARITH_SLOW
#undef BITS_SLOW
#undef CMP_SLOW
}

/* ------------------------------------------------------------------ realms and eval */

static void realm_mark(embi_js_tracer *t, void *user)
{
    embi_js_realm *r = user;
    if (r->global) {
        embi_js_mark_cell(t, &r->global->hdr);
    }
    if (r->eval_fn) {
        embi_js_mark_cell(t, &r->eval_fn->hdr);
    }
    if (r->obj_to_string) {
        embi_js_mark_cell(t, &r->obj_to_string->hdr);
    }
    if (r->thrower) {
        embi_js_mark_cell(t, &r->thrower->hdr);
    }
    if (r->object_ctor) {
        embi_js_mark_cell(t, &r->object_ctor->hdr);
    }
    if (r->lex) {
        embi_js_mark_cell(t, &r->lex->hdr);
    }
    if (r->vars) {
        embi_js_mark_cell(t, &r->vars->hdr);
    }
}

embi_js_realm *embi_js_realm_new(embi_js_runtime *rt)
{
    return embi_js_realm_new_ex(rt, nullptr, nullptr);
}

embi_js_realm *embi_js_realm_new_ex(embi_js_runtime *rt, const embi_js_class *global_class,
                                    void *internal)
{
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    embi_js_realm *r = embi_calloc(1, sizeof *r);
    r->rt = rt;
    embi_js_root_add(rt, realm_mark, r);
    vm->realm = r;
    embi_js_enter(rt);
    embi_js_init_builtins_object(r);
    if (global_class) {
        r->global = embi_js_object_new_class(rt, rt->protos[EMBI_JS_PROTO_OBJECT], global_class);
        r->global->internal = internal;
        EMBI_JS_WB_STRUCT(r->global);
    } else {
        r->global = embi_js_object_new_plain(rt);
    }
    r->global->hdr.flags |= EMBI_JS_OBJ_IS_PROTO; /* J5d: global sites key on its shape (ic.h) */
    r->lex = embi_js_object_new(rt, EMBI_JS_NULL);
    r->vars = embi_js_object_new(rt, EMBI_JS_NULL);
    embi_js_init_builtins_function(r);
    embi_js_init_builtins_error(r);
    embi_js_init_builtins_global(r);
    embi_js_init_builtins_symbol(r);
    embi_js_init_builtins_iterator(r);
    embi_js_init_builtins_array(r);
    embi_js_init_builtins_string(r);
    embi_js_init_builtins_number(r);
    embi_js_init_builtins_boolean(r);
    embi_js_init_builtins_math(r);
    embi_js_init_builtins_json(r);
    embi_js_init_builtins_collections(r);
    embi_js_init_builtins_proxy(r);
    embi_js_init_builtins_promise(r);
    embi_js_init_builtins_regexp(r);
    embi_js_init_builtins_bigint(r);
    embi_js_init_builtins_date(r);
    embi_js_init_builtins_arraybuffer(r);
    embi_js_init_builtins_typedarray(r);
    embi_js_init_builtins_dataview(r);
    embi_js_init_builtins_atomics(r);
    embi_js_init_builtins_weakref(r);
    embi_js_init_builtins_intl(r);
    {
        embi_js_value ots = embi_js_get(rt, embi_js_as_object(rt->protos[EMBI_JS_PROTO_OBJECT]),
                                        embi_js_key_c(rt, "toString"));
        r->obj_to_string = embi_js_is_object(ots) ? embi_js_as_object(ots) : nullptr;
    }
    r->eval_fn = embi_js_native_function_new(r, "eval", 1, js_eval_native, 0);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "eval"), embi_js_from_object(r->eval_fn),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
    embi_js_leave(rt);
    return r;
}

void embi_js_realm_free(embi_js_realm *r)
{
    if (!r) {
        return;
    }
    embi_js_runtime *rt = r->rt;
    embi_js_root_remove(rt, realm_mark, r);
    if (rt->vm && rt->vm->realm == r) {
        rt->vm->realm = nullptr;
    }
    embi_free(r, sizeof *r);
}

embi_js_value embi_js_eval(embi_js_realm *r, const char *src, size_t len, const char *filename,
                           unsigned flags)
{
    embi_js_runtime *rt = r->rt;
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    vm->realm = r;
    if ((uint64_t)len >= 0xFFFFFFFFull) {
        return embi_js_throw_range_error(rt, "Script source is too large");
    }
    embi_js_enter(rt);
    /* No terminating reset here: a nested eval from a native must not clear an unwinding
     * interrupt. closure_call resets it when the outermost frame returns. */
    embi_js_value code = embi_js_compile_ex(r, (const uint8_t *)src, (uint32_t)len, filename,
                                            (flags & EMBI_JS_EVAL_STRICT) != 0,
                                            (flags & EMBI_JS_EVAL_DYNFN) != 0);
    embi_js_value result = EXC;
    if (code != EXC) {
        embi_js_object *f = make_closure(rt, code, nullptr, nullptr);
        result = closure_call(rt, f, embi_js_from_object(r->global), 0, nullptr);
    }
    if (vm->frames.len == 0) {
        vm->terminating = false;
    }
    embi_js_leave(rt);
    return result;
}

void embi_js_set_interrupt(embi_js_runtime *rt, embi_js_interrupt_fn fn, void *user)
{
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    vm->interrupt = fn;
    vm->interrupt_user = user;
}

void embi_js_set_limits(embi_js_runtime *rt, uint32_t max_depth, size_t native_bytes)
{
    embi_js_vm *vm = embi_js_vm_ensure(rt);
    vm->max_depth = max_depth;
    vm->native_limit = native_bytes;
}

/* ------------------------------------------------------------------ module support (J3c)
 *
 * A module's body is the function of its CODE, with no upvalues: its bindings are boxes of its own
 * frame. Calling its closure runs the instantiation, which ends with GEN_START: the call returns
 * the generator object that holds the suspended frame. The linker reads the module's boxes from
 * that frame's slots, and the evaluation resumes the frame at the body.
 */

embi_js_object *embi_jsi_module_closure(embi_js_runtime *rt, embi_js_value code)
{
    embi_js_code *c = code_of(code);
    if (c->nupv != 0) {
        return nullptr;
    }
    return make_closure(rt, code, nullptr, nullptr);
}

embi_js_value embi_jsi_module_instantiate(embi_js_runtime *rt, embi_js_object *closure)
{
    return closure_call(rt, closure, EMBI_JS_UNDEFINED, 0, nullptr);
}

embi_js_value embi_jsi_module_slot(embi_js_value gen, uint32_t slot)
{
    embi_js_gen *g = gen_of(embi_js_as_object(gen));
    if (slot >= g->nsaved) {
        return EMBI_JS_EMPTY;
    }
    return g->saved[slot];
}

embi_js_value embi_jsi_module_resume(embi_js_runtime *rt, embi_js_value gen)
{
    embi_js_object *o = embi_js_as_object(gen);
    embi_js_gen *g = gen_of(o);
    embi_js_vm *vm = rt->vm;
    if (!native_ok(rt, vm) || vm->frames.len >= vm->max_depth) {
        return embi_js_throw_range_error(rt, "Maximum call stack size exceeded");
    }
    embi_js_value r = gen_enter(rt, o, 0, EMBI_JS_UNDEFINED);
    if (r == EXC) {
        g->state = EMBI_JS_GEN_COMPLETED;
        gen_release(g);
        return EXC;
    }
    if (g->susp == EMBI_JS_SUSP_NONE) {
        g->state = EMBI_JS_GEN_COMPLETED;
        gen_release(g);
    }
    return r;
}
