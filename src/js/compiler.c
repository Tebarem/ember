/*
 * Compiler core: bytecode emission with stack-depth tracking, arena-backed compile structures,
 * the unit's GC root, and the CODE builder that turns finished functions into CODE cells.
 */
#include "js/compiler.h"
#include "js/ic.h"
#include "js/interp.h"
#include "js/ops.h"

#include <string.h>

#define EMBI_JS_STACK_SLACK 2

const embi_js_op_info embi_js_op_table[EMBI_JSOP_COUNT] = {
#define OP(name, size, pops, pushes) [EMBI_JSOP_##name] = {#name, size, pops, pushes},
#include "js/opcodes.def"
#undef OP
};

/* ------------------------------------------------------------------ arena-backed structures */

void *embi_jsc_alloc(cunit *p, size_t n)
{
    void *m = embi_arena_alloc(&p->arena, n, 16);
    if (n <= 128) {
        /* Small records: whole 16-byte stores (the arena rounds nothing, but the caller's record
         * is at most n bytes and the next allocation is 16-aligned), no library call. */
        unsigned char *d = m;
        size_t i = 0;
        for (; i + 16 <= n; i += 16) {
            __builtin_memset(d + i, 0, 16);
        }
        for (; i < n; i++) {
            d[i] = 0;
        }
    } else {
        memset(m, 0, n);
    }
    return m;
}

void *embi_jsc_vgrow(embi_arena *ar, void *data, uint32_t *cap, size_t elem)
{
    uint32_t old = *cap;
    uint32_t ncap = old ? old * 2u : (elem == 1 ? 128u : 8u);
    if (data && ar->last) {
        embi_arena_block *b = ar->last;
        size_t extra = (size_t)(ncap - old) * elem;
        if ((unsigned char *)data + (size_t)old * elem == b->data + b->used && extra <= b->cap - b->used) {
            b->used += extra;
            *cap = ncap;
            return data;
        }
    }
    void *nd = embi_arena_alloc(ar, (size_t)ncap * elem, 16);
    if (old) {
        memcpy(nd, data, (size_t)old * elem);
    }
    *cap = ncap;
    return nd;
}

cfunc *embi_jsc_new_func(cunit *p, cfunc *parent, cscope *def)
{
    cfunc *f = embi_jsc_alloc(p, sizeof *f);
    f->ar = &p->arena;
    f->parent = parent;
    f->def = def;
    f->root = embi_jsc_new_scope(p, nullptr);
    f->cur = f->root;
    f->arguments_slot = EMBI_JS_NO_SLOT;
    f->strict = parent ? parent->strict : false;
    if (parent) {
        f->dynref = parent->dynref;
        for (const cscope *s = def; s && !f->dynref; s = s->parent) {
            f->dynref = s->kind == SC_WITH;
        }
    }
    f->name = EMBI_JS_EMPTY;
    f->self_name = EMBI_JS_EMPTY;
    f->code_value = EMBI_JS_EMPTY;
    embi_vec_push(&p->all, f);
    return f;
}

cscope *embi_jsc_new_scope(cunit *p, cscope *parent)
{
    cscope *s = embi_jsc_alloc(p, sizeof *s);
    s->parent = parent;
    return s;
}

#define SCOPE_HASH_MIN 16u

static uint32_t name_hash(embi_js_value v)
{
    return (uint32_t)(((uint64_t)v * 0x9E3779B97F4A7C15ull) >> 32);
}

static void scope_table_insert(cbind **table, uint32_t cap, cbind *b)
{
    uint32_t i = name_hash(b->name) & (cap - 1u);
    while (table[i]) {
        if (table[i]->name == b->name) {
            return; /* the first binding of a name wins, as in the list scan */
        }
        i = (i + 1u) & (cap - 1u);
    }
    table[i] = b;
}

/* FP2a: the reference resolver asks every scope on the way out whether it declares a name, and almost all of them do not:
 * one bit test answers without walking the binding list. */
static inline uint64_t bloom_bit(embi_js_value name)
{
    return 1ull << ((name_hash(name) >> 8) & 63u);
}

static void scope_add(cunit *p, cscope *s, cbind *b)
{
    s->bloom |= bloom_bit(b->name);
    if (s->last) {
        s->last->next_in_scope = b;
    } else {
        s->first = b;
    }
    s->last = b;
    s->count++;
    if (s->count <= SCOPE_HASH_MIN) {
        return;
    }
    if (s->cap == 0 || s->count * 2u > s->cap) {
        uint32_t cap = s->cap ? s->cap * 2u : 64u;
        while (cap < s->count * 2u) {
            cap *= 2u;
        }
        cbind **t = embi_jsc_alloc(p, (size_t)cap * sizeof *t);
        for (cbind *x = s->first; x; x = x->next_in_scope) {
            scope_table_insert(t, cap, x);
        }
        s->table = t;
        s->cap = cap;
    } else {
        scope_table_insert(s->table, s->cap, b);
    }
}

cbind *embi_jsc_new_bind(cunit *p, cfunc *f, cscope *s, embi_js_value name, uint8_t kind)
{
    cbind *b = embi_jsc_alloc(p, sizeof *b);
    b->name = name;
    b->kind = kind;
    b->slot = -1;
    b->scope = s;
    b->owner = f;
    if (s) {
        scope_add(p, s, b);
    }
    CF_PUSH(f, binds, b);
    return b;
}

cbind *embi_jsc_find_in_scope(cscope *s, embi_js_value name)
{
    if (!s || (s->bloom & bloom_bit(name)) == 0) {
        return nullptr;
    }
    if (s->cap) {
        uint32_t i = name_hash(name) & (s->cap - 1u);
        while (s->table[i]) {
            if (s->table[i]->name == name) {
                return s->table[i];
            }
            i = (i + 1u) & (s->cap - 1u);
        }
        return nullptr;
    }
    for (cbind *b = s->first; b; b = b->next_in_scope) {
        if (b->name == name) {
            return b;
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ emission */

static void adjust_depth(cunit *p, int pops, int pushes)
{
    cfunc *f = p->fn;
    f->depth -= pops;
    /* A negative depth means the emitter's model of the operand stack has drifted from the real
     * one, and the frame sizes would then be wrong. Never clamp it away. */
    EMBI_ASSERT(f->depth >= 0);
    f->depth += pushes;
    if (f->depth > f->max_depth) {
        f->max_depth = f->depth;
    }
}

uint32_t embi_jsc_here(const cunit *p)
{
    return (uint32_t)p->fn->code.len;
}

/* Appends n zero bytes to the function's code and returns the offset of the first. */
static uint32_t code_extend(cfunc *f, uint32_t n)
{
    uint32_t pc = f->code.len;
    /* Room for n bytes plus the 16-byte store below, which may run past the new end. */
    while (pc + n + 16u > f->code.cap) {
        f->code.data = embi_jsc_vgrow(f->ar, f->code.data, &f->code.cap, 1);
    }
    if (n <= 16u) {
        __builtin_memset(f->code.data + pc, 0, 16);
    } else {
        memset(f->code.data + pc, 0, n);
    }
    f->code.len = pc + n;
    return pc;
}

uint32_t embi_jsc_emit_op(cunit *p, uint8_t op)
{
    const embi_js_op_info *info = &embi_js_op_table[op];
    cfunc *f = p->fn;
    uint32_t pc = code_extend(f, (uint32_t)info->size);
    f->code.data[pc] = op;
    adjust_depth(p, info->pops, info->pushes);
    return pc;
}

/* The ops whose operand is a key constant that becomes an inline cache site (J5c, ic.h). */
static bool op_has_ic(uint8_t op)
{
    switch (op) {
    case EMBI_JSOP_GET_FIELD:
    case EMBI_JSOP_SET_FIELD:
    case EMBI_JSOP_GET_FIELD_THIS:
    case EMBI_JSOP_DEFINE_FIELD:
    case EMBI_JSOP_SUPER_GET:
        return true;
    default:
        return false;
    }
}

uint32_t embi_jsc_global_ic(cfunc *f, uint32_t name_k)
{
    CF_PUSH(f, ics, name_k);
    return f->ics.len - 1u;
}

uint32_t embi_jsc_emit_op32(cunit *p, uint8_t op, uint32_t v)
{
    if (op_has_ic(op)) {
        /* v is the key's constant index. The operand becomes the site's index, and build_one
         * resolves the key of each site. */
        CF_PUSH(p->fn, ics, v);
        v = p->fn->ics.len - 1u;
    }
    uint32_t pc = embi_jsc_emit_op(p, op);
    embi_js_wr32(&p->fn->code.data[pc + 1], v);
    return pc;
}

uint32_t embi_jsc_grow(cunit *p, uint32_t n)
{
    return code_extend(p->fn, n);
}

uint32_t embi_jsc_emit_op2(cunit *p, uint8_t op, uint32_t a, uint32_t b)
{
    const embi_js_op_info *info = &embi_js_op_table[op];
    uint32_t pc = code_extend(p->fn, (uint32_t)info->size);
    p->fn->code.data[pc] = op;
    embi_js_wr32(&p->fn->code.data[pc + 1], a);
    embi_js_wr32(&p->fn->code.data[pc + 5], b);
    adjust_depth(p, info->pops, info->pushes);
    return pc;
}

uint32_t embi_jsc_emit_op_eff(cunit *p, uint8_t op, uint32_t v, int pops, int pushes)
{
    uint32_t pc = code_extend(p->fn, 5);
    p->fn->code.data[pc] = op;
    embi_js_wr32(&p->fn->code.data[pc + 1], v);
    adjust_depth(p, pops, pushes);
    return pc;
}

void embi_jsc_patch(cunit *p, uint32_t operand_at, uint32_t target)
{
    embi_js_wr32(&p->fn->code.data[operand_at], target);
}

void embi_jsc_patch_list(cunit *p, cpatch *list, uint32_t target)
{
    for (cpatch *n = list; n; n = n->next) {
        embi_jsc_patch(p, n->at, target);
    }
}

cpatch *embi_jsc_patch_add(cunit *p, cpatch *list, uint32_t at)
{
    cpatch *n = embi_jsc_alloc(p, sizeof *n);
    n->at = at;
    n->next = list;
    return n;
}

uint32_t embi_jsc_add_const(cunit *p, embi_js_value v)
{
    uint32_t idx = (uint32_t)p->fn->consts.len;
    CF_PUSH(p->fn, consts, v);
    return idx;
}

void embi_jsc_mark_pos(cunit *p, uint32_t line, uint32_t col)
{
    cfunc *f = p->fn;
    uint32_t pc = embi_jsc_here(p);
    if (f->pos.len > 0) {
        embi_js_pos_entry *last = &f->pos.data[f->pos.len - 1];
        if (last->pc == pc) {
            last->line = line;
            last->col = col;
            return;
        }
        if (last->line == line && last->col == col) {
            return;
        }
    }
    embi_js_pos_entry e = {pc, line, col};
    CF_PUSH(f, pos, e);
}

void embi_jsc_emit_sym(cunit *p, uint8_t kind, embi_js_value name, cscope *scope, cbind *direct)
{
    static const uint8_t ops[] = {
        [SYM_KIND_GET] = EMBI_JSOP_SYM_GET,
        [SYM_KIND_SET] = EMBI_JSOP_SYM_SET,
        [SYM_KIND_INIT] = EMBI_JSOP_SYM_INIT,
        [SYM_KIND_TYPEOF] = EMBI_JSOP_SYM_TYPEOF,
        [SYM_KIND_DELETE] = EMBI_JSOP_SYM_DELETE,
        [SYM_KIND_CALL] = EMBI_JSOP_SYM_CALL,
        [SYM_KIND_REF] = EMBI_JSOP_SYM_REF,
        [SYM_KIND_GETREF] = EMBI_JSOP_SYM_GETREF,
        [SYM_KIND_SETREF] = EMBI_JSOP_SYM_SETREF,
    };
    cfunc *f = p->fn;
    uint32_t idx = (uint32_t)f->syms.len;
    uint32_t pc = embi_jsc_emit_op32(p, ops[kind], idx);
    csym s = {name, scope, direct, nullptr, -1, -1, pc, kind};
    CF_PUSH(f, syms, s);
}

/* ------------------------------------------------------------------ unit lifetime */

void embi_jsc_unit_root(embi_js_tracer *t, void *user)
{
    const cunit *p = user;
    for (uint32_t i = 0; i < p->all.len; i++) {
        const cfunc *f = p->all.data[i];
        for (uint32_t j = 0; j < f->consts.len; j++) {
            embi_js_mark_value(t, f->consts.data[j]);
        }
        embi_js_mark_value(t, f->code_value);
        embi_js_mark_value(t, f->name);
        embi_js_mark_value(t, f->self_name);
    }
    for (uint32_t i = 0; i < p->npending; i++) {
        embi_js_mark_value(t, p->pending_labels[i]);
    }
}

void embi_jsc_unit_free(cunit *p)
{
    embi_vec_free(&p->all);
    embi_hashmap_free(&p->grp);
    embi_hashmap_free(&p->arr);
    embi_vec_free(&p->bm);
    embi_vec_free(&p->gstk);
    embi_arena_free(&p->arena);
    embi_free(p->scratch, (size_t)p->scratch_cap * sizeof(uint16_t));
    embi_free(p->peek_scratch, (size_t)p->peek_cap * sizeof(uint16_t));
    if (p->file) {
        embi_free(p->file, strlen(p->file) + 1u);
        p->file = nullptr;
    }
}

/* ------------------------------------------------------------------ CODE cells */

bool embi_js_code_position(const embi_js_code *c, uint32_t pc, uint32_t *line, uint32_t *col)
{
    if (c->pos_bytes == 0) {
        return false;
    }
    /* J5n2: the table is a byte stream, one entry per (pc delta, zigzag line delta, column), each a
     * LEB128 number. The lookup is a linear decode: it runs only when an error is made or a position is
     * asked for, never on the execution path. The result is the last entry at or before pc, or the
     * first entry when pc lies before all of them. */
    const uint8_t *p = c->pos;
    const uint8_t *end = c->pos + c->pos_bytes;
    uint32_t cur_pc = 0;
    uint32_t cur_line = 0;
    uint32_t best_line = 0;
    uint32_t best_col = 0;
    for (uint32_t i = 0; p < end; i++) {
        uint32_t v[3];
        for (int k = 0; k < 3; k++) {
            uint32_t x = 0;
            unsigned shift = 0;
            while (p < end) {
                uint8_t b = *p++;
                x |= (uint32_t)(b & 0x7fu) << shift;
                if (!(b & 0x80u)) {
                    break;
                }
                shift += 7;
            }
            v[k] = x;
        }
        cur_pc += v[0];
        cur_line += (v[1] >> 1) ^ (0u - (v[1] & 1u));
        if (i > 0 && cur_pc > pc) {
            break;
        }
        best_line = cur_line;
        best_col = v[2];
    }
    *line = best_line;
    *col = best_col;
    return true;
}

/* The size in bytes of x as an unsigned LEB128 number. */
static uint32_t leb_size(uint32_t x)
{
    uint32_t n = 1;
    while (x >= 0x80u) {
        x >>= 7;
        n++;
    }
    return n;
}

/* The position entries of a function are encoded as described at embi_js_code_position.
 * positions_size gives the exact byte count; encode_positions writes that many bytes to buf. */
static uint32_t positions_size(const embi_js_pos_entry *e, uint32_t n)
{
    uint32_t len = 0;
    uint32_t prev_pc = 0;
    uint32_t prev_line = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t dl = e[i].line - prev_line;
        uint32_t zz = (dl << 1) ^ (uint32_t)((int32_t)dl >> 31);
        len += leb_size(e[i].pc - prev_pc) + leb_size(zz) + leb_size(e[i].col);
        prev_pc = e[i].pc;
        prev_line = e[i].line;
    }
    return len;
}

static void encode_positions(const embi_js_pos_entry *e, uint32_t n, uint8_t *buf)
{
    size_t len = 0;
    uint32_t prev_pc = 0;
    uint32_t prev_line = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t dl = e[i].line - prev_line;
        uint32_t zz = (dl << 1) ^ (uint32_t)((int32_t)dl >> 31);
        uint32_t v[3] = {e[i].pc - prev_pc, zz, e[i].col};
        for (int k = 0; k < 3; k++) {
            uint32_t x = v[k];
            while (x >= 0x80u) {
                buf[len++] = (uint8_t)(x | 0x80u);
                x >>= 7;
            }
            buf[len++] = (uint8_t)x;
        }
        prev_pc = e[i].pc;
        prev_line = e[i].line;
    }
}

void embi_js_code_trace(embi_js_cell *cell, embi_js_tracer *t)
{
    embi_js_code *c = (embi_js_code *)cell;
    embi_js_mark_value(t, c->name);
    for (uint32_t i = 0; i < c->nconsts; i++) {
        embi_js_mark_value(t, c->consts[i]);
    }
    embi_js_ic_trace(c->ics, c->nics, t);
    if (c->ic_keys) {
        for (uint32_t i = 0; i < c->nics; i++) {
            embi_js_mark_value(t, c->ic_keys[i]);
        }
        t->external += (size_t)c->nics * sizeof(embi_js_value);
    }
    if (c->fn_from) {
        embi_js_mark_cell(t, &c->fn_from->hdr);
        embi_js_mark_cell(t, &c->fn_to->hdr);
    }
    t->external += c->bc_len + c->nconsts * sizeof(embi_js_value) +
                   c->nexc * sizeof(embi_js_exc_entry) + (size_t)c->pos_bytes +
                   c->nupv * sizeof(embi_js_upval_desc) + (c->file ? strlen(c->file) + 1u : 0u) +
                   c->ndents * sizeof(embi_js_dyn_ent) + c->neents * sizeof(embi_js_eval_ent);
}

static_assert(sizeof(embi_js_code) <= 256, "J5n2: a code cell fits the 256-byte size class");

void embi_js_code_ics_ensure(embi_js_runtime *rt, embi_js_code *c)
{
    if (c->ics != nullptr || c->nics == 0) {
        return;
    }
    embi_js_ic *ics = embi_js_ic_alloc(c->nics);
    for (uint32_t i = 0; i < c->nics; i++) {
        embi_js_ic_init(rt, &ics[i], c->ic_keys[i]);
    }
    c->ics = ics;
    embi_js_heap_note_external(&rt->heap, (size_t)c->nics * sizeof(embi_js_ic));
    /* An old code cell does not point at a young cell through this array (it holds only keys, which
     * ic_keys already keeps alive), so no barrier is needed. */
}

void embi_js_code_finalize(embi_js_cell *cell)
{
    embi_js_code *c = (embi_js_code *)cell;
    embi_js_ic_free(c->ics, c->nics);
    c->ics = nullptr;
    c->nics = 0;
    if (c->src_buf && --c->src_buf->refs == 0) {
        embi_free(c->src_buf, sizeof(embi_js_srcbuf) + (size_t)c->src_buf->len);
    }
    c->src_buf = nullptr;
    c->src = nullptr;
    /* FP1a: the file name and every side table are in the one block. */
    embi_free(c->blk, c->blk_len);
    c->blk = nullptr;
    c->blk_len = 0;
    c->dents = nullptr;
    c->dsites = nullptr;
    c->eents = nullptr;
    c->enames = nullptr;
    c->esites = nullptr;
    c->pmap = nullptr;
    c->file = nullptr;
    c->bc = nullptr;
    c->consts = nullptr;
    c->exc = nullptr;
    c->pos = nullptr;
    c->upv = nullptr;
    c->ic_keys = nullptr;
}

/* An upvalue of f is a global when following its parent chain ends at a global. */
static bool upval_is_global(const cfunc *f, uint32_t idx)
{
    for (;;) {
        const cupval *e = &f->upvals.data[idx];
        if (e->kind == UPV_GLOBAL) {
            return true;
        }
        if (e->kind != UPV_PARENT || !f->parent) {
            return false;
        }
        idx = e->index;
        f = f->parent;
    }
}

/* The binding an upvalue of f refers to, when its parent chain ends at a local binding. */
static const cbind *upval_bind(const cfunc *f, uint32_t idx)
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

void embi_jsc_write_callee_store(cfunc *f, uint32_t pc)
{
    if (f->strict) {
        f->code.data[pc] = EMBI_JSOP_CONST_ASSIGN;
        embi_js_wr32(&f->code.data[pc + 1], 0);
    } else {
        /* A jump to the next op keeps the stored value on the stack and changes nothing. */
        f->code.data[pc] = EMBI_JSOP_JMP;
        embi_js_wr32(&f->code.data[pc + 1], pc + 5u);
    }
}

static void rewrite_global_sites(cunit *p, cfunc *f)
{
    (void)p;
    for (uint32_t i = 0; i < f->upsites.len; i++) {
        const cupsite *s = &f->upsites.data[i];
        if (s->kind == SYM_KIND_SET) {
            const cbind *b = upval_bind(f, s->upidx);
            if (b && b->kind == BK_CALLEE) {
                embi_jsc_write_callee_store(f, s->pc);
                continue;
            }
        }
        if (!upval_is_global(f, s->upidx)) {
            continue;
        }
        uint8_t op;
        switch (s->kind) {
        case SYM_KIND_GET:
        case SYM_KIND_CALL:
            op = EMBI_JSOP_GET_GLOBAL;
            break;
        case SYM_KIND_TYPEOF:
            op = EMBI_JSOP_GET_GLOBAL_OPT;
            break;
        case SYM_KIND_SET:
            op = EMBI_JSOP_SET_GLOBAL;
            break;
        case SYM_KIND_DELETE:
            op = EMBI_JSOP_DELETE_GLOBAL;
            break;
        default:
            EMBI_UNREACHABLE();
        }
        uint32_t k = (uint32_t)f->consts.len;
        CF_PUSH(f, consts, s->name);
        if (op != EMBI_JSOP_DELETE_GLOBAL) {
            k = embi_jsc_global_ic(f, k);
        }
        f->code.data[s->pc] = op;
        embi_js_wr32(&f->code.data[s->pc + 1], k);
    }
}

/* ------------------------------------------------------------------ superinstructions (J5i) */

/* EMBI_JS_FUSE_MASK selects the pairs of embi_js_fuse_defs the peephole may emit (bit i for entry
 * i). It defaults to all of them; 0 turns the pass off. The compile definition sets the default of
 * the runtime copy embi_jsc_fuse_mask, which the unit tests change to compare fused and unfused
 * code in one binary. */
#ifndef EMBI_JS_FUSE_MASK
#define EMBI_JS_FUSE_MASK ((1u << EMBI_JS_NFUSE) - 1u)
#endif
uint32_t embi_jsc_fuse_mask = EMBI_JS_FUSE_MASK;

const embi_js_fuse_def embi_js_fuse_defs[EMBI_JS_NFUSE] = {
    {EMBI_JSOP_GET_LOC, EMBI_JSOP_PUSH_I32, EMBI_JSOP_GET_LOC_I32},
    {EMBI_JSOP_INC, EMBI_JSOP_SET_LOC, EMBI_JSOP_INC_SET_LOC},
};

/* Ops that only fall through to the next op and never save or move the frame's pc. The successor
 * of any other op can be entered by a jump, a resumption or a handler, so the peephole starts a
 * new block there. A throw reaches a handler through the exception table, which is checked too. */
/* The classes of ops for the peephole: FUSE_FALL falls through to the next op and never moves the pc
 * (its successor is not a block start); every other op ends a block. The jump classes also name
 * their targets: one operand at +1, two at +1 and +5, or the dynamic-site resume point at +6. */
enum { FUSE_OTHER = 0, FUSE_FALL = 1, FUSE_JUMP = 2, FUSE_JUMP2 = 3, FUSE_DYN = 4 };
static const uint8_t fuse_cls[EMBI_JSOP_COUNT] = {
    [EMBI_JSOP_NOP] = FUSE_FALL,
    [EMBI_JSOP_POP] = FUSE_FALL,
    [EMBI_JSOP_DUP] = FUSE_FALL,
    [EMBI_JSOP_DUP2] = FUSE_FALL,
    [EMBI_JSOP_ROT3] = FUSE_FALL,
    [EMBI_JSOP_ROT4] = FUSE_FALL,
    [EMBI_JSOP_SWAP] = FUSE_FALL,
    [EMBI_JSOP_PUSH_UNDEF] = FUSE_FALL,
    [EMBI_JSOP_PUSH_NULL] = FUSE_FALL,
    [EMBI_JSOP_PUSH_TRUE] = FUSE_FALL,
    [EMBI_JSOP_PUSH_FALSE] = FUSE_FALL,
    [EMBI_JSOP_PUSH_I32] = FUSE_FALL,
    [EMBI_JSOP_PUSH_CONST] = FUSE_FALL,
    [EMBI_JSOP_PUSH_THIS] = FUSE_FALL,
    [EMBI_JSOP_PUSH_CALLEE] = FUSE_FALL,
    [EMBI_JSOP_PUSH_EMPTY] = FUSE_FALL,
    [EMBI_JSOP_GET_LOC] = FUSE_FALL,
    [EMBI_JSOP_SET_LOC] = FUSE_FALL,
    [EMBI_JSOP_PUT_LOC] = FUSE_FALL,
    [EMBI_JSOP_GET_BOX] = FUSE_FALL,
    [EMBI_JSOP_SET_BOX] = FUSE_FALL,
    [EMBI_JSOP_GET_UPVAL] = FUSE_FALL,
    [EMBI_JSOP_SET_UPVAL] = FUSE_FALL,
    [EMBI_JSOP_GET_FIELD] = FUSE_FALL,
    [EMBI_JSOP_SET_FIELD] = FUSE_FALL,
    [EMBI_JSOP_GET_FIELD_THIS] = FUSE_FALL,
    [EMBI_JSOP_GET_INDEX] = FUSE_FALL,
    [EMBI_JSOP_SET_INDEX] = FUSE_FALL,
    [EMBI_JSOP_ADD] = FUSE_FALL,
    [EMBI_JSOP_SUB] = FUSE_FALL,
    [EMBI_JSOP_MUL] = FUSE_FALL,
    [EMBI_JSOP_DIV] = FUSE_FALL,
    [EMBI_JSOP_MOD] = FUSE_FALL,
    [EMBI_JSOP_SHL] = FUSE_FALL,
    [EMBI_JSOP_SAR] = FUSE_FALL,
    [EMBI_JSOP_SHR] = FUSE_FALL,
    [EMBI_JSOP_BAND] = FUSE_FALL,
    [EMBI_JSOP_BOR] = FUSE_FALL,
    [EMBI_JSOP_BXOR] = FUSE_FALL,
    [EMBI_JSOP_EQ] = FUSE_FALL,
    [EMBI_JSOP_NE] = FUSE_FALL,
    [EMBI_JSOP_SEQ] = FUSE_FALL,
    [EMBI_JSOP_SNE] = FUSE_FALL,
    [EMBI_JSOP_LT] = FUSE_FALL,
    [EMBI_JSOP_GT] = FUSE_FALL,
    [EMBI_JSOP_LE] = FUSE_FALL,
    [EMBI_JSOP_GE] = FUSE_FALL,
    [EMBI_JSOP_NEG] = FUSE_FALL,
    [EMBI_JSOP_NOT] = FUSE_FALL,
    [EMBI_JSOP_BNOT] = FUSE_FALL,
    [EMBI_JSOP_TYPEOF] = FUSE_FALL,
    [EMBI_JSOP_TO_NUMERIC] = FUSE_FALL,
    [EMBI_JSOP_INC] = FUSE_FALL,
    [EMBI_JSOP_DEC] = FUSE_FALL,
    [EMBI_JSOP_TO_STRING] = FUSE_FALL,
    [EMBI_JSOP_JMP] = FUSE_JUMP,
    [EMBI_JSOP_JMP_ENTRY] = FUSE_JUMP,
    [EMBI_JSOP_SWITCH_JMP] = FUSE_JUMP,
    [EMBI_JSOP_JMP_TRUE] = FUSE_JUMP,
    [EMBI_JSOP_JMP_FALSE] = FUSE_JUMP,
    [EMBI_JSOP_JMP_TRUE_KEEP] = FUSE_JUMP,
    [EMBI_JSOP_JMP_FALSE_KEEP] = FUSE_JUMP,
    [EMBI_JSOP_JMP_NOT_NULLISH_KEEP] = FUSE_JUMP,
    [EMBI_JSOP_OPT_NULLISH] = FUSE_JUMP,
    [EMBI_JSOP_FOR_IN_NEXT] = FUSE_JUMP,
    [EMBI_JSOP_ITER_NEXT] = FUSE_JUMP,
    [EMBI_JSOP_GEN_RESUME] = FUSE_JUMP,
    [EMBI_JSOP_ASYNC_ITER_STEP] = FUSE_JUMP,
    [EMBI_JSOP_ASYNC_CLOSE_CALL] = FUSE_JUMP,
    [EMBI_JSOP_ASYNC_RET_PREP] = FUSE_JUMP,
    [EMBI_JSOP_DELEGATE_STEP] = FUSE_JUMP2,
    [EMBI_JSOP_ASYNC_DELEGATE_CALL] = FUSE_JUMP2,
    [EMBI_JSOP_ASYNC_DELEGATE_CHECK] = FUSE_JUMP2,
    [EMBI_JSOP_DYN_GET] = FUSE_DYN,
    [EMBI_JSOP_DYN_TYPEOF] = FUSE_DYN,
    [EMBI_JSOP_DYN_SET] = FUSE_DYN,
    [EMBI_JSOP_DYN_DELETE] = FUSE_DYN,
    [EMBI_JSOP_DYN_CALL] = FUSE_DYN,
    [EMBI_JSOP_DYN_REF] = FUSE_DYN,
    [EMBI_JSOP_DYN_GET_REF] = FUSE_DYN,
    [EMBI_JSOP_DYN_SET_REF] = FUSE_DYN,
};

static void fuse_mark(uint8_t *mark, uint32_t len, uint32_t at)
{
    if (at <= len) {
        mark[at] = 1;
    }
}

/* Rewrites adjacent op pairs of f's bytecode into the fused ops of embi_js_fuse_defs. A fused op
 * keeps the byte size of its two parts, so no offset moves: the bytecode, the exception table,
 * the position table and the dynamic sites keep their pcs. A pair is fused only when its second
 * part can't be entered from anywhere else: no jump or dynamic-site target, no resumption point and
 * no protected-range boundary lands on it. Jump operands are read from the same ops the interpreter
 * jumps with (interp.c), and the successor of every op that does not fall through is a block
 * start. Position entries don't matter: a handler whose second part can throw or call out moves pc
 * to that part before doing so, so error locations and stack frames see the unfused pc. */
static void embi_jsc_fuse(cfunc *f)
{
    uint32_t len = (uint32_t)f->code.len;
    uint8_t *code = f->code.data;
    uint32_t mask = embi_jsc_fuse_mask;
    if (len == 0 || mask == 0) {
        return;
    }
    /* Small functions use a stack mark array, so the common case allocates nothing. */
    uint8_t small[512];
    uint8_t *mark = len < sizeof small ? small : embi_alloc((size_t)len + 1u);
    memset(mark, 0, (size_t)len + 1u);
    /* FP1a: one walk finds the block starts and the candidate pairs together. The pairs are applied
     * afterwards, in order, against the finished marks. The list starts on the stack and moves to
     * the heap when a function has more pairs than it holds. */
    uint32_t cand_small[64];
    uint32_t *cand = cand_small;
    uint32_t cand_cap = (uint32_t)(sizeof cand_small / sizeof cand_small[0]);
    uint32_t ncand = 0;
    for (uint32_t p = 0; p < len;) {
        uint8_t op = code[p];
        uint32_t sz = embi_js_op_table[op].size;
        EMBI_ASSERT(sz > 0 && p + sz <= len);
        uint8_t cls = fuse_cls[op];
        if (cls != FUSE_FALL) {
            fuse_mark(mark, len, p + sz);
            switch (cls) {
            case FUSE_JUMP:
                fuse_mark(mark, len, embi_js_rd32(code + p + 1));
                break;
            case FUSE_JUMP2:
                fuse_mark(mark, len, embi_js_rd32(code + p + 1));
                fuse_mark(mark, len, embi_js_rd32(code + p + 5));
                break;
            case FUSE_DYN:
                /* Some dynamic sites resume two bytes past their successor (the skipped call form). */
                fuse_mark(mark, len, p + 6u);
                break;
            default:
                break;
            }
        }
        if (p + sz < len) {
            uint8_t next = code[p + sz];
            for (uint32_t i = 0; i < EMBI_JS_NFUSE; i++) {
                if (((mask >> i) & 1u) && embi_js_fuse_defs[i].first == op &&
                    embi_js_fuse_defs[i].second == next) {
                    if (ncand == cand_cap) {
                        uint32_t *bigger = embi_alloc((size_t)cand_cap * 2u * sizeof *cand);
                        memcpy(bigger, cand, (size_t)ncand * sizeof *cand);
                        if (cand != cand_small) {
                            embi_free(cand, (size_t)cand_cap * sizeof *cand);
                        }
                        cand = bigger;
                        cand_cap *= 2u;
                    }
                    cand[ncand++] = p;
                    break;
                }
            }
        }
        p += sz;
    }
    if (ncand != 0) {
        for (uint32_t i = 0; i < f->exc.len; i++) {
            const embi_js_exc_entry *e = &f->exc.data[i];
            fuse_mark(mark, len, e->start);
            fuse_mark(mark, len, e->end);
            fuse_mark(mark, len, e->handler);
        }
        for (uint32_t i = 0; i < f->dsites.len; i++) {
            fuse_mark(mark, len, f->dsites.data[i].fallback);
        }
        uint32_t free_at = 0; /* the first pc not yet covered by a fused pair */
        for (uint32_t c = 0; c < ncand; c++) {
            uint32_t p = cand[c];
            if (p < free_at) {
                continue;
            }
            uint8_t op = code[p];
            uint32_t sz = embi_js_op_table[op].size;
            if (mark[p + sz]) {
                continue;
            }
            uint8_t next = code[p + sz];
            for (uint32_t i = 0; i < EMBI_JS_NFUSE; i++) {
                if (((mask >> i) & 1u) && embi_js_fuse_defs[i].first == op &&
                    embi_js_fuse_defs[i].second == next) {
                    EMBI_ASSERT(sz + embi_js_op_table[next].size ==
                                embi_js_op_table[embi_js_fuse_defs[i].fused].size);
                    code[p] = embi_js_fuse_defs[i].fused;
                    free_at = p + sz + embi_js_op_table[next].size;
                    break;
                }
            }
        }
    }
    if (cand != cand_small) {
        embi_free(cand, (size_t)cand_cap * sizeof *cand);
    }
    if (mark != small) {
        embi_free(mark, (size_t)len + 1u);
    }
}

static void build_one(cunit *p, cfunc *f)
{
    embi_jsc_tdz_upsites(f);
    rewrite_global_sites(p, f);
    /* Names of eval contexts become constants before the constant table is copied. */
    for (uint32_t i = 0; i < f->enames.len; i++) {
        cdynname *n = &f->enames.data[i];
        n->name_k = (uint32_t)f->consts.len;
        CF_PUSH(f, consts, n->name);
    }

    for (uint32_t i = 0; i < f->children.len; i++) {
        cfunc *c = f->children.data[i];
        f->consts.data[c->const_idx] = c->code_value;
    }

    embi_js_runtime *rt = p->rt;
    embi_js_code *c = (embi_js_code *)embi_js_alloc_cell(rt, sizeof *c, EMBI_JS_CELL_CODE);
    c->name = f->name;

    const char *file = p->file ? p->file : "";
    size_t file_len = strlen(file);

    embi_jsc_fuse(f);

    /* FP1a: every side table of the cell lives in one allocation (c->blk), laid out here. A malloc
     * and a free per table dominated the build on Windows (about 130 ns a pair). An empty table is
     * a nullptr. */
    const uint32_t n_bc = (uint32_t)f->code.len;
    const uint32_t n_k = (uint32_t)f->consts.len;
    const uint32_t n_exc = (uint32_t)f->exc.len;
    const uint32_t n_pos_bytes = positions_size(f->pos.data, (uint32_t)f->pos.len);
    const uint32_t n_upv = (uint32_t)f->upvals.len;
    const uint32_t n_dents = (uint32_t)f->dents.len;
    const uint32_t n_dsites = (uint32_t)f->dsites.len;
    const uint32_t n_eents = (uint32_t)f->eents.len;
    const uint32_t n_enames = (uint32_t)f->enames.len;
    const uint32_t n_esites = (uint32_t)f->evals.len;
    const uint32_t n_pmap = f->mapped ? f->nparams : 0;
    const uint32_t n_ics = (uint32_t)f->ics.len;
    size_t off = 0;
#define LAY(var, T, n)                                                                         \
    size_t var = (off + _Alignof(T) - 1u) & ~(size_t)(_Alignof(T) - 1u);                      \
    off = var + (size_t)(n) * sizeof(T)
    LAY(o_k, embi_js_value, n_k);
    LAY(o_ic, embi_js_value, n_ics);
    LAY(o_exc, embi_js_exc_entry, n_exc);
    LAY(o_upv, embi_js_upval_desc, n_upv);
    LAY(o_dents, embi_js_dyn_ent, n_dents);
    LAY(o_dsites, embi_js_dyn_site, n_dsites);
    LAY(o_eents, embi_js_eval_ent, n_eents);
    LAY(o_enames, embi_js_eval_name, n_enames);
    LAY(o_esites, embi_js_eval_site, n_esites);
    LAY(o_bc, uint8_t, n_bc);
    LAY(o_pos, uint8_t, n_pos_bytes);
    LAY(o_pmap, uint8_t, n_pmap);
    LAY(o_file, char, file_len + 1u);
#undef LAY
    uint8_t *blk = embi_alloc(off);
    c->blk = blk;
    c->blk_len = (uint32_t)off;
#define SLOT(T, o, n) ((n) ? (T *)(blk + (o)) : nullptr)
    c->file = (char *)(blk + o_file);
    memcpy(c->file, file, file_len + 1u);

    c->bc_len = n_bc;
    c->bc = blk + o_bc;
    memcpy(c->bc, f->code.data, n_bc);

    c->nconsts = n_k;
    c->consts = SLOT(embi_js_value, o_k, n_k);
    for (uint32_t i = 0; i < n_k; i++) {
        c->consts[i] = f->consts.data[i];
    }

    c->nexc = n_exc;
    c->exc = SLOT(embi_js_exc_entry, o_exc, n_exc);
    if (n_exc) {
        memcpy(c->exc, f->exc.data, (size_t)n_exc * sizeof(embi_js_exc_entry));
    }

    c->pos_bytes = n_pos_bytes;
    c->pos = SLOT(uint8_t, o_pos, n_pos_bytes);
    encode_positions(f->pos.data, (uint32_t)f->pos.len, c->pos);
#ifndef NDEBUG
    /* Debug builds check the delta code against the table it came from: every entry must decode to
     * itself, whatever order the compiler emitted them in. */
    for (uint32_t i = 0; i < f->pos.len; i++) {
        uint32_t want_line = 0;
        uint32_t want_col = 0;
        for (uint32_t j = 0; j < f->pos.len; j++) {
            if (j == 0 || f->pos.data[j].pc <= f->pos.data[i].pc) {
                want_line = f->pos.data[j].line;
                want_col = f->pos.data[j].col;
            }
        }
        uint32_t got_line = 0;
        uint32_t got_col = 0;
        bool have = embi_js_code_position(c, f->pos.data[i].pc, &got_line, &got_col);
        EMBI_ASSERT(have && got_line == want_line && got_col == want_col);
    }
#endif

    c->nupv = n_upv;
    c->upv = SLOT(embi_js_upval_desc, o_upv, n_upv);
    for (uint32_t i = 0; i < c->nupv; i++) {
        const cupval *e = &f->upvals.data[i];
        embi_js_upval_desc d = {0, EMBI_JS_UPV_NONE};
        if (e->kind == UPV_LOCAL) {
            d.index = (uint32_t)e->bind->slot;
            d.from = EMBI_JS_UPV_LOCAL;
        } else if (e->kind == UPV_PARENT) {
            d.index = e->index;
            d.from = EMBI_JS_UPV_PARENT;
        }
        c->upv[i] = d;
    }

    /* Dynamic scope tables. */
    c->ndents = n_dents;
    c->dents = SLOT(embi_js_dyn_ent, o_dents, n_dents);
    for (uint32_t i = 0; i < c->ndents; i++) {
        const cdynent *d = &f->dents.data[i];
        c->dents[i] = (embi_js_dyn_ent){d->type, d->src, d->index};
    }
    c->ndsites = n_dsites;
    c->dsites = SLOT(embi_js_dyn_site, o_dsites, n_dsites);
    for (uint32_t i = 0; i < c->ndsites; i++) {
        const cdynsite *d = &f->dsites.data[i];
        c->dsites[i] = (embi_js_dyn_site){d->name, d->fallback, d->first, d->n, d->unres};
    }
    c->neents = n_eents;
    c->eents = SLOT(embi_js_eval_ent, o_eents, n_eents);
    for (uint32_t i = 0; i < c->neents; i++) {
        const cdynent *d = &f->eents.data[i];
        c->eents[i] = (embi_js_eval_ent){d->type, d->src, d->index, d->first_name, d->nnames};
    }
    c->nenames = n_enames;
    c->enames = SLOT(embi_js_eval_name, o_enames, n_enames);
    for (uint32_t i = 0; i < c->nenames; i++) {
        const cdynname *n = &f->enames.data[i];
        c->enames[i] = (embi_js_eval_name){n->name_k, n->src, n->index, n->ro, n->tok};
    }
    c->nesites = n_esites;
    c->esites = SLOT(embi_js_eval_site, o_esites, n_esites);
    for (uint32_t i = 0; i < c->nesites; i++) {
        const ceval *e = &f->evals.data[i];
        c->esites[i] = (embi_js_eval_site){e->argc, e->name_k, e->first, e->n, e->strict, e->nt_ok, e->args_bad,
                                           e->this_up < 0 ? EMBI_JS_NO_SLOT : (uint32_t)e->this_up,
                                           e->nt_up < 0 ? EMBI_JS_NO_SLOT : (uint32_t)e->nt_up,
                                           e->super_ok, e->priv_ok,
                                           e->home_up < 0 ? EMBI_JS_NO_SLOT : (uint32_t)e->home_up,
                                           e->noargs, e->mod_k};
    }

    c->nparams = f->nparams;
    c->nlocals = f->nlocals;
    c->max_stack = (uint32_t)f->max_depth + EMBI_JS_STACK_SLACK;
    c->arguments_slot = f->arguments_slot;
    c->fn_length = f->length;
    c->rest_slot = f->has_rest ? f->rest_slot : 0;
    c->src = nullptr;
    c->src_len = 0;
    c->src_buf = nullptr;
    if (!embi_jsc_is_top(f) && f->src_end > f->src_start) {
        /* One copy of the unit's text serves every function of it (a cell keeps only its span). */
        if (!p->srcbuf) {
            p->srcbuf = embi_alloc(sizeof(embi_js_srcbuf) + (size_t)p->src_len);
            p->srcbuf->refs = 0;
            p->srcbuf->len = p->src_len;
            memcpy(p->srcbuf->data, p->src, p->src_len);
        }
        p->srcbuf->refs++;
        c->src_buf = p->srcbuf;
        c->src = p->srcbuf->data + f->src_start;
        c->src_len = f->src_end - f->src_start;
    }
    c->flags = 0;
    if (f->has_rest) {
        c->flags |= EMBI_JS_CODE_REST;
    }
    if (f->strict) {
        c->flags |= EMBI_JS_CODE_STRICT;
    }
    if (f->is_script) {
        c->flags |= EMBI_JS_CODE_SCRIPT;
    }
    if (f->is_eval) {
        c->flags |= EMBI_JS_CODE_EVAL;
    }
    if (f->is_class_ctor) {
        c->flags |= EMBI_JS_CODE_CONSTRUCTOR | EMBI_JS_CODE_CLASS_CTOR;
        if (f->is_derived) {
            c->flags |= EMBI_JS_CODE_DERIVED;
        }
    } else if (f->is_arrow || f->is_method || f->is_gen || f->is_async) {
        c->flags |= EMBI_JS_CODE_NO_SELF;
    } else if (!embi_jsc_is_top(f)) {
        c->flags |= EMBI_JS_CODE_CONSTRUCTOR;
    }
    if (f->is_class_ctor || f->is_method || f->is_field_init || f->is_static_block) {
        c->flags |= EMBI_JS_CODE_HOMED;
    }
    if (f->is_gen) {
        c->flags |= EMBI_JS_CODE_GENERATOR;
    }
    if (f->is_async) {
        c->flags |= EMBI_JS_CODE_ASYNC;
    }
    c->npmap = n_pmap;
    c->pmap = SLOT(uint8_t, o_pmap, n_pmap);
    for (uint32_t i = 0; i < c->npmap; i++) {
        const cbind *b = i < f->pnames.len ? embi_jsc_find_in_scope(f->root, f->pnames.data[i])
                                           : nullptr;
        c->pmap[i] = (b && b->slot == (int32_t)i) ? 1u : 0u;
    }
    if (f->mapped) {
        c->flags |= EMBI_JS_CODE_MAPPED_ARGS;
    }
    /* Inline caches last: key interning may collect, and the cell is complete by now. */
    c->nics = n_ics;
    c->ic_keys = SLOT(embi_js_value, o_ic, n_ics);
    for (uint32_t i = 0; i < c->nics; i++) {
        c->ic_keys[i] = EMBI_JS_UNDEFINED; /* the cell is traced while the next key is interned */
    }
    for (uint32_t i = 0; i < c->nics; i++) {
        embi_js_value kv = f->consts.data[f->ics.data[i]];
        c->ic_keys[i] = embi_js_key_from_str(rt, embi_js_as_string(kv));
    }
    f->code_value = embi_js_from_cell(c);
#undef SLOT
}

embi_js_value embi_jsc_build(cunit *p)
{
    /* Children are appended after their parents, so building in reverse order builds every
     * child before the parent that needs its CODE value. */
    for (uint32_t i = p->all.len; i-- > 0;) {
        build_one(p, p->all.data[i]);
    }
    return p->top->code_value;
}
