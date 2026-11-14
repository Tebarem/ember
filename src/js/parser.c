/*
 * Recursive-descent parser that emits bytecode directly (no AST). Expressions are returned as
 * descriptors (expr) so that assignment targets and call receivers can be emitted correctly:
 * a reference stays unloaded until the parser knows how it is used.
 *
 * Control flow. Every try statement gets a finally context, even without a finally block. A
 * finally block is compiled once. Exits that must run it (break, continue, return) store a
 * completion kind in the try's temporary and jump to it, and the block dispatches on the kind
 * afterwards. Kind 0 is normal completion, 1 is a throw (the value is rethrown), and each exit
 * route gets its own id from 2 upwards.
 *
 * Unsupported ES2015+ syntax reports "not yet supported: <feature>" as a SyntaxError.
 */
#include "js/compiler.h"
#include "js/interp.h"
#include "js/regexp.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/string.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <stdarg.h>
#include <string.h>

/* EX_SUPER_FIELD, EX_SUPER_INDEX and EX_PRIV are references whose base operands are on the
 * stack: [this home] (with the key on top for EX_SUPER_INDEX) and [obj key] for a private name. */
typedef enum expr_kind { EX_VALUE, EX_VAR, EX_FIELD, EX_INDEX, EX_SUPER_FIELD, EX_SUPER_INDEX,
                         EX_PRIV } expr_kind;

typedef struct expr {
    uint8_t kind;      /* expr_kind */
    bool paren;        /* parenthesized */
    embi_js_value name; /* EX_VAR: identifier atom. EX_FIELD: key atom */
    cscope *scope;     /* EX_VAR: scope of the reference */
    uint32_t key;      /* EX_FIELD: constant index of the key */
    uint32_t line;     /* position of the token that produced the reference */
    uint32_t col;
    uint32_t start;    /* byte offset of the first token of the expression (callee text) */
    uint8_t literal;   /* an array or object literal (a destructuring pattern if assigned) */
    bool anon;         /* an anonymous function or arrow definition (names a computed key) */
    cpatch *opt;       /* EX_FIELD/EX_INDEX of an optional chain kept unloaded: its short jumps */
} expr;

#define OPC(op) embi_jsc_emit_op(p, EMBI_JSOP_##op)
#define OPC32(op, v) embi_jsc_emit_op32(p, EMBI_JSOP_##op, (uint32_t)(v))

/* ------------------------------------------------------------------ errors and tokens */

/* Reports an error of the given type at the current token and returns false. */
static bool perr_as(cunit *p, enum embi_js_proto which, const char *fmt, va_list ap)
{
    char buf[320];
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    (void)embi_js_throw_error(p->rt, which, "%s", buf);
    embi_js_note_location(p->rt, p->file, p->tok.line, p->tok.col);
    return false;
}

static bool perr_plain(cunit *p, enum embi_js_proto which, const char *msg)
{
    (void)embi_js_throw_error(p->rt, which, "%s", msg);
    embi_js_note_location(p->rt, p->file, p->tok.line, p->tok.col);
    return false;
}

static bool perr(cunit *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));


static bool is_eval_or_args(const cunit *p, embi_js_value nm)
{
    return nm == p->atom_eval || nm == p->atom_arguments;
}

static bool is_strict_word(const cunit *p, embi_js_value nm)
{
    for (uint32_t i = 0; i < 9; i++) {
        if (nm == p->atom_strict[i]) {
            return true;
        }
    }
    return false;
}

/* A binding or assignment target that strict code may not name eval or arguments. */
static bool check_strict_name(cunit *p, embi_js_value nm)
{
    if (p->fn->strict && is_eval_or_args(p, nm)) {
        return perr(p, "Unexpected eval or arguments in strict mode");
    }
    return true;
}

/* An assignment or increment target: in strict code it may not be eval or arguments. */
static bool check_target(cunit *p, const expr *e)
{
    if (e->kind == EX_VAR) {
        return check_strict_name(p, e->name);
    }
    return true;
}

/* A fresh internal name (it contains '%', so no source identifier can spell it). */
static embi_js_value tmp_name(cunit *p, char kind)
{
    char buf[24];
    SDL_snprintf(buf, sizeof buf, "%%%c%u", kind, (unsigned)p->tmp_counter++);
    return embi_js_from_string(embi_js_atom_intern_utf8(p->rt, buf, strlen(buf)));
}

static bool has_completion(const cfunc *f)
{
    return embi_jsc_is_top(f) && f->completion;
}

static bool perr(cunit *p, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool r = perr_as(p, EMBI_JS_PROTO_SYNTAX_ERROR, fmt, ap);
    va_end(ap);
    return r;
}

static bool unexpected(cunit *p)
{
    if (p->tok.kind == EMBI_JST_ERROR) {
        return perr(p, "%s", p->lx.err ? p->lx.err : "Invalid or unexpected token");
    }
    if (p->tok.kind == EMBI_JST_EOF) {
        return perr(p, "Unexpected end of input");
    }
    return perr(p, "Unexpected token");
}

/* The scratch buffers start small and double when a token does not fit (a token needs at most
 * as many units as its byte length, so growing to the source length always suffices). The scan
 * is repeated from a saved lexer state. peek_buf selects the lookahead buffer, which never
 * holds the current token's text. */
static void lex_scan(cunit *p, embi_jslex *lx, embi_jstok *out, bool peek_buf)
{
    for (;;) {
        /* The state a failed scan must be able to return to (a token longer than the scratch). */
        const uint32_t s_pos = lx->pos;
        const uint32_t s_pc_pos = lx->pc_pos;
        const uint32_t s_pc_line = lx->pc_line;
        const uint32_t s_pc_col = lx->pc_col;
        const bool s_regex_ok = lx->regex_ok;
        const char *const s_err = lx->err;
        const uint32_t s_err_pos = lx->err_pos;
        const bool s_failed = lx->failed;
        embi_jslex_next(lx, out);
        if (out->kind != EMBI_JST_ERROR || !lx->err ||
            strcmp(lx->err, "token too long for the lexer scratch buffer") != 0) {
            return;
        }
        uint16_t **buf = peek_buf ? &p->peek_scratch : &p->scratch;
        uint32_t *cap = peek_buf ? &p->peek_cap : &p->scratch_cap;
        if (*cap >= p->src_len + 1u) {
            return;
        }
        uint32_t ncap = *cap * 2u;
        if (ncap > p->src_len + 1u || ncap < *cap) {
            ncap = p->src_len + 1u;
        }
        embi_free(*buf, (size_t)*cap * sizeof(uint16_t));
        *buf = embi_alloc((size_t)ncap * sizeof(uint16_t));
        *cap = ncap;
        lx->pos = s_pos;
        lx->pc_pos = s_pc_pos;
        lx->pc_line = s_pc_line;
        lx->pc_col = s_pc_col;
        lx->regex_ok = s_regex_ok;
        lx->err = s_err;
        lx->err_pos = s_err_pos;
        lx->failed = s_failed;
        lx->scratch = *buf;
        lx->scratch_cap = ncap;
    }
}

static void adv(cunit *p)
{
    p->last_end = p->tok.end;
    if (p->pk_valid && p->pk_from == p->lx.pos && !p->lx.failed && !p->lx.regex_ok) {
        /* The text was lexed by the peek() before (a pure function of the offset). */
        p->tok = p->pk_tok;
        p->lx.pos = p->pk_lx.pos;
        p->lx.pc_pos = p->pk_lx.pc_pos;
        p->lx.pc_line = p->pk_lx.pc_line;
        p->lx.pc_col = p->pk_lx.pc_col;
        p->pk_valid = false;
        return;
    }
    lex_scan(p, &p->lx, &p->tok, false);
}

/* ------------------------------------------------------------------ nesting limit */

/* Every construct that recurses (expressions, unary chains, nested statements, function bodies)
 * takes a guard. The count bounds the depth, and the native stack budget bounds the C stack the
 * recursion uses, whichever runs out first. */
#define EMBI_JS_PARSE_MAX_NEST 10000

typedef struct nest_guard {
    cunit *p;
} nest_guard;

static bool nest_enter(cunit *p)
{
    p->nest++;
    if (p->nest > EMBI_JS_PARSE_MAX_NEST) {
        return perr(p, "Maximum nesting depth exceeded");
    }
    /* FP1a: the stack probe costs a thread-local lookup; every fourth level is enough, since the
     * budget (768 KB) is many times the stack four parser levels use. */
    if ((p->nest & 3) == 0 && !embi_js_native_stack_ok(p->rt)) {
        return perr_plain(p, EMBI_JS_PROTO_RANGE_ERROR, "Maximum call stack size exceeded");
    }
    return true;
}

static void nest_exit(nest_guard *g)
{
    g->p->nest--;
}

/* Use at the top of a recursive parse function; the guard unwinds on every return path. */
#define NEST_GUARD(p_)                                                                         \
    nest_guard nest_g_ __attribute__((cleanup(nest_exit))) = {p_};                             \
    if (!nest_enter(p_))                                                                       \
        return false

static bool is(const cunit *p, embi_jstok_kind k)
{
    return p->tok.kind == k;
}

static bool is_kw(const cunit *p, embi_jskw kw)
{
    return p->tok.kind == EMBI_JST_NAME && p->tok.kw == kw && !(p->tok.flags & EMBI_JST_F_ESCAPED);
}

static bool expect(cunit *p, embi_jstok_kind k)
{
    if (p->tok.kind != k) {
        return unexpected(p);
    }
    adv(p);
    return true;
}

/* Lookahead without disturbing the current token: the copy scans into its own scratch. */
static void peek(cunit *p, embi_jstok *out)
{
    embi_jslex lx = p->lx;
    lx.scratch = p->peek_scratch;
    lx.scratch_cap = p->peek_cap;
    lex_scan(p, &lx, out, true);
    p->pk_valid = out->kind != EMBI_JST_ERROR && out->cooked == nullptr && !lx.failed &&
                  !p->lx.regex_ok;
    if (p->pk_valid) {
        p->pk_tok = *out;
        p->pk_lx = lx;
        p->pk_from = p->lx.pos;
    }
}

static bool reserved_kw(embi_jskw kw)
{
    switch (kw) {
    case EMBI_JSKW_break:
    case EMBI_JSKW_case:
    case EMBI_JSKW_catch:
    case EMBI_JSKW_class:
    case EMBI_JSKW_const:
    case EMBI_JSKW_continue:
    case EMBI_JSKW_debugger:
    case EMBI_JSKW_default:
    case EMBI_JSKW_delete:
    case EMBI_JSKW_do:
    case EMBI_JSKW_else:
    case EMBI_JSKW_enum:
    case EMBI_JSKW_export:
    case EMBI_JSKW_extends:
    case EMBI_JSKW_false:
    case EMBI_JSKW_finally:
    case EMBI_JSKW_for:
    case EMBI_JSKW_function:
    case EMBI_JSKW_if:
    case EMBI_JSKW_import:
    case EMBI_JSKW_in:
    case EMBI_JSKW_instanceof:
    case EMBI_JSKW_new:
    case EMBI_JSKW_null:
    case EMBI_JSKW_return:
    case EMBI_JSKW_super:
    case EMBI_JSKW_switch:
    case EMBI_JSKW_this:
    case EMBI_JSKW_throw:
    case EMBI_JSKW_true:
    case EMBI_JSKW_try:
    case EMBI_JSKW_typeof:
    case EMBI_JSKW_var:
    case EMBI_JSKW_void:
    case EMBI_JSKW_while:
    case EMBI_JSKW_with:
        return true;
    default:
        return false;
    }
}

static bool strict_reserved_kw(embi_jskw kw)
{
    switch (kw) {
    case EMBI_JSKW_implements:
    case EMBI_JSKW_interface:
    case EMBI_JSKW_let:
    case EMBI_JSKW_package:
    case EMBI_JSKW_private:
    case EMBI_JSKW_protected:
    case EMBI_JSKW_public:
    case EMBI_JSKW_static:
    case EMBI_JSKW_yield:
        return true;
    default:
        return false;
    }
}

/* True where `await` is reserved as an identifier: in a class static block, and in arrow
 * parameters of arrow functions nested in one (ClassStaticBlockBody is parsed with [+Await]).
 * An arrow body or an ordinary function nested in the block ends the walk. */
static bool await_reserved(const cunit *p)
{
    /* J3c: await is a reserved word throughout module code. */
    if (p->top && p->top->is_module) {
        return true;
    }
    for (const cfunc *g = p->fn; g; g = g->parent) {
        if (g->is_static_block || g->is_async) {
            return true;
        }
        /* An arrow's parameters inherit [Await]; its body is parsed with [~Await]. */
        if (!g->is_arrow || !g->in_params) {
            return false;
        }
    }
    return false;
}

/* True where `yield` is reserved as an identifier because of a generator: in the generator itself
 * and in the parameters of arrow functions nested in it (arrow parameters inherit [Yield]). */
static bool yield_reserved(const cunit *p)
{
    for (const cfunc *g = p->fn; g; g = g->parent) {
        if (g->is_gen) {
            return true;
        }
        if (!g->is_arrow || !g->in_params) {
            return false;
        }
    }
    return false;
}

/* True when the current token can be a binding or reference identifier. */
static bool is_ident(const cunit *p)
{
    if (p->tok.kind != EMBI_JST_NAME) {
        return false;
    }
    if (p->tok.kw == EMBI_JSKW_NONE) {
        return true;
    }
    if (p->tok.kw == EMBI_JSKW_await && await_reserved(p)) {
        return false;
    }
    if (p->tok.kw == EMBI_JSKW_yield && yield_reserved(p)) {
        return false;
    }
    if (reserved_kw(p->tok.kw)) {
        return false;
    }
    if (p->fn->strict && strict_reserved_kw(p->tok.kw)) {
        return false;
    }
    return true;
}

/* The atom for the current NAME token (IdentifierName, keywords included). */
static embi_js_value name_atom(cunit *p)
{
    if (p->tok.flags & EMBI_JST_F_ESCAPED) {
        embi_js_string *s = embi_js_str_new_utf16(p->rt, p->tok.cooked, p->tok.cooked_len);
        return embi_js_from_string(embi_js_atom_intern(p->rt, s));
    }
    if (p->na_start == p->tok.start && p->na_end == p->tok.end) {
        return p->na_atom;
    }
    embi_js_value a = embi_js_from_string(embi_js_atom_intern_utf8(
        p->rt, (const char *)p->src + p->tok.start, p->tok.end - p->tok.start));
    p->na_start = p->tok.start;
    p->na_end = p->tok.end;
    p->na_atom = a;
    return a;
}

static uint32_t const_str(cunit *p, embi_js_value v)
{
    return embi_jsc_add_const(p, v);
}

/* The value of the BigInt literal at the current token (J4b). Returns EMBI_JS_EXCEPTION when the
 * literal is too large, with the RangeError pending. */
static embi_js_value bigint_token_value(cunit *p)
{
    uint32_t n = embi_jslex_bigint_digits(&p->lx, &p->tok, nullptr, 0);
    size_t bytes = (size_t)n + 1u;
    char *buf = embi_alloc(bytes);
    (void)embi_jslex_bigint_digits(&p->lx, &p->tok, buf, n);
    embi_js_bigint *b = embi_js_bigint_from_digits(p->rt, buf, n, p->tok.radix, false);
    embi_free(buf, bytes);
    return b ? embi_js_from_bigint(b) : EMBI_JS_EXCEPTION;
}

/* The source text of a callee, kept as a constant for the "is not a function" message. Returns
 * 1 + the constant index, or 0 when there is no usable text (too long, or the index does not
 * fit the call operand). */
static uint32_t callee_text_const(cunit *p, uint32_t start, uint32_t end)
{
    if (end <= start || end - start > 80u) {
        return 0;
    }
    /* Interned, so repeated callees share one string; a small direct-mapped cache per function
     * also shares the constant slot. */
    char buf[80];
    size_t bn = 0;
    bool in_ws = false;
    for (uint32_t i = start; i < end; i++) {
        char c = (char)p->src[i];
        if (c == 32 || c == 9 || c == 10 || c == 13) {
            in_ws = true;
            continue;
        }
        if (in_ws && bn > 0) {
            buf[bn++] = ' ';
        }
        in_ws = false;
        buf[bn++] = c;
    }
    if (bn == 0) {
        return 0;
    }
    embi_js_value a = embi_js_from_string(embi_js_atom_intern_utf8(p->rt, buf, bn));
    cfunc *f = p->fn;
    uint32_t h = (uint32_t)(((uint64_t)a >> 4) * 0x9E3779B1u >> 24) & (EMBI_JSC_CALLEE_CACHE - 1u);
    uint32_t k;
    if (f->cc_val[h] == a && f->cc_used[h]) {
        k = f->cc_idx[h];
    } else {
        k = const_str(p, a);
        f->cc_val[h] = a;
        f->cc_idx[h] = k;
        f->cc_used[h] = true;
    }
    return k + 1u < 0xFFFFu ? k + 1u : 0;
}

static void mark(cunit *p, uint32_t line, uint32_t col)
{
    embi_jsc_mark_pos(p, line, col);
}

/* ------------------------------------------------------------------ expression references */

static void to_value(cunit *p, expr *e)
{
    switch (e->kind) {
    case EX_VAR:
        mark(p, e->line, e->col);
        embi_jsc_emit_sym(p, SYM_KIND_GET, e->name, e->scope, nullptr);
        break;
    case EX_FIELD:
        mark(p, e->line, e->col);
        OPC32(GET_FIELD, e->key);
        break;
    case EX_INDEX:
        mark(p, e->line, e->col);
        OPC(GET_INDEX);
        break;
    case EX_SUPER_FIELD:
        OPC32(SUPER_GET, e->key);
        break;
    case EX_SUPER_INDEX:
        OPC(SUPER_GET_INDEX);
        break;
    case EX_PRIV:
        mark(p, e->line, e->col);
        OPC(PRIV_GET);
        break;
    default:
        break;
    }
    if (e->opt) {
        /* A parenthesized optional chain that was kept as a reference: its short-circuit jumps
         * land here, with undefined as the value. */
        embi_jsc_patch_list(p, e->opt, embi_jsc_here(p));
        e->opt = nullptr;
    }
    e->kind = EX_VALUE;
}

/* Resets e to a plain computed value. Clears every cover-grammar flag (paren, literal): callers
 * pass uninitialised out-parameters, so no flag may survive from a previous use. */
static void set_value(expr *e)
{
    memset(e, 0, sizeof *e);
    e->kind = EX_VALUE;
}

static bool assignable(const expr *e)
{
    return (e->kind == EX_VAR || e->kind == EX_FIELD || e->kind == EX_INDEX ||
            e->kind == EX_SUPER_FIELD || e->kind == EX_SUPER_INDEX || e->kind == EX_PRIV) &&
           !e->opt;
}

/* Whether a name reference may cross a with object or an eval context, so that it has to be
 * resolved before its right-hand side is evaluated (the ref ops of opcodes.def). */
static bool early_ref(const cunit *p, const expr *e)
{
    if (e->kind != EX_VAR) {
        return false;
    }
    if (p->fn->dynref) {
        return true;
    }
    for (const cscope *s = e->scope; s; s = s->parent) {
        if (s->kind == SC_WITH) {
            return true;
        }
    }
    return false;
}

/* Whether the name is declared in the scope s or in any scope that encloses it (also across the
 * enclosing functions). A strict assignment to a name that is not (yet) visible is resolved
 * before its right-hand side runs: the right-hand side may create the global, which does not make
 * the reference resolvable (PutValue on an unresolvable reference, ECMA-262 6.2.5.6). */
static bool name_visible(const cunit *p, const cscope *s, embi_js_value name)
{
    for (const cfunc *g = p->fn; g; g = g->parent) {
        for (const cscope *sc = g == p->fn ? s : g->def; sc; sc = sc->parent) {
            if (embi_jsc_find_in_scope((cscope *)sc, name)) {
                return true;
            }
        }
        if (g->self_name == name) {
            return true;
        }
    }
    return false;
}

/* Read-modify-write prologue: duplicates the base (and key) that a later get and set consume. */
static void ref_dup(cunit *p, const expr *e)
{
    if (e->kind == EX_FIELD) {
        OPC(DUP);
    } else if (e->kind == EX_INDEX) {
        OPC(CHECK_BASE);
        OPC(TO_PROPKEY); /* the reference converts its key once, before the get and the set */
        OPC(DUP2);
    } else if (e->kind == EX_SUPER_FIELD || e->kind == EX_PRIV) {
        OPC(DUP2);
    } else if (e->kind == EX_SUPER_INDEX) {
        OPC(SUPER_BASE); /* the base is fixed once, before the key converts, for the get and the set */
        OPC(TO_PROPKEY);
        OPC(DUP3);
    }
}

static void ref_get(cunit *p, const expr *e)
{
    switch (e->kind) {
    case EX_VAR:
        embi_jsc_emit_sym(p, SYM_KIND_GET, e->name, e->scope, nullptr);
        break;
    case EX_FIELD:
        OPC32(GET_FIELD, e->key);
        break;
    case EX_SUPER_FIELD:
        OPC32(SUPER_GET, e->key);
        break;
    case EX_SUPER_INDEX:
        OPC(SUPER_GET_INDEX);
        break;
    case EX_PRIV:
        OPC(PRIV_GET);
        break;
    default:
        OPC(GET_INDEX);
        break;
    }
}

/* Stores the value on top. The value stays on the stack as the expression's result. */
static void ref_set(cunit *p, const expr *e)
{
    switch (e->kind) {
    case EX_VAR:
        embi_jsc_emit_sym(p, SYM_KIND_SET, e->name, e->scope, nullptr);
        break;
    case EX_FIELD:
        OPC32(SET_FIELD, e->key);
        break;
    case EX_SUPER_FIELD:
        OPC32(SUPER_SET, e->key);
        break;
    case EX_SUPER_INDEX:
        OPC(SUPER_SET_INDEX);
        break;
    case EX_PRIV:
        OPC(PRIV_SET);
        break;
    default:
        OPC(SET_INDEX);
        break;
    }
}

/* Emits the operator for a compound assignment or an increment. */
static void emit_postfix(cunit *p, const expr *e, bool inc)
{
    uint8_t step = inc ? EMBI_JSOP_INC : EMBI_JSOP_DEC;
    if (early_ref(p, e)) {
        embi_jsc_emit_sym(p, SYM_KIND_REF, e->name, e->scope, nullptr);
        embi_jsc_emit_sym(p, SYM_KIND_GETREF, e->name, e->scope, nullptr);
        OPC(TO_NUMERIC);
        OPC(DUP);
        OPC(ROT3); /* [h v v] -> [v h v] */
        embi_jsc_emit_op(p, step);
        embi_jsc_emit_sym(p, SYM_KIND_SETREF, e->name, e->scope, nullptr);
        OPC(POP);
        return;
    }
    ref_dup(p, e);
    ref_get(p, e);
    OPC(TO_NUMERIC);
    if (e->kind == EX_VAR) {
        OPC(DUP);
        embi_jsc_emit_op(p, step);
        ref_set(p, e);
        OPC(POP);
    } else if (e->kind == EX_FIELD) {
        OPC(DUP);
        OPC(ROT3);
        embi_jsc_emit_op(p, step);
        ref_set(p, e);
        OPC(POP);
    } else if (e->kind == EX_SUPER_INDEX) {
        OPC(DUP);
        OPC(ROT5); /* [this home key old old] -> [old this home key old] */
        embi_jsc_emit_op(p, step);
        ref_set(p, e);
        OPC(POP);
    } else {
        OPC(DUP);
        OPC(ROT4);
        embi_jsc_emit_op(p, step);
        ref_set(p, e);
        OPC(POP);
    }
}

static void emit_prefix(cunit *p, const expr *e, bool inc)
{
    if (early_ref(p, e)) {
        embi_jsc_emit_sym(p, SYM_KIND_REF, e->name, e->scope, nullptr);
        embi_jsc_emit_sym(p, SYM_KIND_GETREF, e->name, e->scope, nullptr);
        embi_jsc_emit_op(p, inc ? EMBI_JSOP_INC : EMBI_JSOP_DEC);
        embi_jsc_emit_sym(p, SYM_KIND_SETREF, e->name, e->scope, nullptr);
        return;
    }
    ref_dup(p, e);
    ref_get(p, e);
    embi_jsc_emit_op(p, inc ? EMBI_JSOP_INC : EMBI_JSOP_DEC);
    ref_set(p, e);
}

/* Jump with a patch site. Returns the operand offset to patch. */
static uint32_t jump_emit(cunit *p, uint8_t op)
{
    return embi_jsc_emit_op32(p, op, 0) + 1u;
}

static void jump_to(cunit *p, uint32_t operand_at, uint32_t target)
{
    embi_jsc_patch(p, operand_at, target);
}

/* ------------------------------------------------------------------ control stack */

static uint32_t ctl_items(const cunit *p)
{
    uint32_t n = 0;
    for (const ctl *c = p->ctl; c; c = c->parent) {
        n += c->items;
    }
    return n;
}

static cjumprec *fin_route(cunit *p, ctl *fin, ctl *target, bool cont, bool is_return)
{
    for (cjumprec *r = fin->jumps; r; r = r->next) {
        if (r->target == target && r->cont == cont && r->is_return == is_return) {
            return r;
        }
    }
    cjumprec *r = embi_jsc_alloc(p, sizeof *r);
    r->id = fin->next_id++;
    r->target = target;
    r->cont = cont;
    r->is_return = is_return;
    r->next = fin->jumps;
    fin->jumps = r;
    return r;
}

static void tmp_get(cunit *p, cbind *t);

/* Drops the items a construct keeps on the operand stack when control leaves it. A for-of keeps
 * its iterator record: leaving it closes the iterator (IteratorClose on a normal completion). An
 * array pattern keeps its record in a temporary (J3e): leaving it by a return completion closes it
 * the same way, with the errors of return propagated. */
static void pop_items(cunit *p, const ctl *c)
{
    if (c->pat_rec) {
        tmp_get(p, c->pat_rec);
        OPC(ITER_CLOSE);
    }
    for (uint8_t i = 0; i < c->items; i++) {
        if (c->is_iter && c->is_async_iter) {
            /* A for-await: the return call and its awaited result (ASYNC_CLOSE_CALL jumps past them
             * when there is no return method). */
            uint32_t jl = jump_emit(p, EMBI_JSOP_ASYNC_CLOSE_CALL);
            OPC(AWAIT);
            OPC32(GEN_RESUME, 0);
            OPC(ASYNC_CLOSE_CHECK);
            jump_to(p, jl, embi_jsc_here(p));
        } else if (c->is_iter) {
            OPC(ITER_CLOSE_KEEP);
        }
        OPC(POP);
    }
}

static void emit_this_raw(cunit *p);

/* The final return of the function with the value on top. A derived constructor first applies the
 * constructor result rules (CTOR_RETURN), after every finally block has run. */
static void emit_ctor_check(cunit *p)
{
    if (p->fn->is_derived) {
        emit_this_raw(p);
        OPC(CTOR_RETURN);
    }
}

static void emit_fn_return(cunit *p)
{
    emit_ctor_check(p);
    OPC(RETURN);
}

/* Leaves the constructs between p->ctl and target (target nullptr means the function). Each
 * crossed for-in or switch drops its operand, and each crossed for-of closes its iterator. Reaching
 * a finally block routes the exit through it. */
static void emit_unwind(cunit *p, ctl *target, bool cont, bool is_return)
{
    int32_t saved = p->fn->depth;
    ctl *c = p->ctl;
    for (; c != target; c = c->parent) {
        if (c->kind == CTL_TRY_FIN) {
            cjumprec *r = fin_route(p, c, target, cont, is_return);
            OPC32(PUSH_I32, r->id);
            embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, c->fk);
            uint32_t j = jump_emit(p, EMBI_JSOP_JMP);
            c->fin_sites = embi_jsc_patch_add(p, c->fin_sites, j);
            p->fn->depth = saved;
            return;
        }
        pop_items(p, c);
    }
    if (!target) {
        emit_fn_return(p);
    } else if (cont) {
        uint32_t j = jump_emit(p, EMBI_JSOP_JMP);
        target->cont = embi_jsc_patch_add(p, target->cont, j);
    } else {
        pop_items(p, target);
        uint32_t j = jump_emit(p, EMBI_JSOP_JMP);
        target->brk = embi_jsc_patch_add(p, target->brk, j);
    }
    p->fn->depth = saved;
}

/* A return with the value on top. Crossing a finally block routes it through the block. Crossing
 * a for-of closes its iterator, so the value moves to a temporary while the iterators close. It restores the depth with
 * the value still counted, which yield relies on (its resumption joins with a value); a return statement must restore its
 * own depth from before the value afterwards (JFOROF). */
static void emit_return_top(cunit *p)
{
    int32_t saved = p->fn->depth;
    ctl *c = p->ctl;
    while (c && c->kind != CTL_TRY_FIN) {
        c = c->parent;
    }
    bool iter = false;
    for (ctl *d = p->ctl; d && d != c; d = d->parent) {
        iter = iter || d->is_iter || d->pat_rec;
    }
    if (c) {
        embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, c->fv);
        emit_unwind(p, nullptr, false, true);
    } else if (!iter) {
        emit_fn_return(p);
    } else {
        cbind *t = embi_jsc_new_bind(p, p->fn, nullptr, EMBI_JS_EMPTY, BK_TEMP);
        embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, t);
        for (ctl *d = p->ctl; d; d = d->parent) {
            pop_items(p, d);
        }
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t);
        if (p->fn->is_derived) {
            /* The result check may throw, and no for-of handler may see that throw (the iterators
             * are closed): it runs at the function epilogue, outside every protected range. */
            uint32_t j = jump_emit(p, EMBI_JSOP_JMP);
            p->fn->epi = embi_jsc_patch_add(p, p->fn->epi, j);
        } else {
            OPC(RETURN);
        }
    }
    p->fn->depth = saved;
}

static void reset_completion(cunit *p)
{
    if (has_completion(p->fn)) {
        OPC(PUSH_UNDEF);
        embi_jsc_emit_sym(p, SYM_KIND_SET, EMBI_JS_EMPTY, nullptr, p->fn->completion);
        OPC(POP);
    }
}

static void take_labels(cunit *p, ctl *c)
{
    c->nlabels = p->npending;
    for (uint8_t i = 0; i < p->npending; i++) {
        c->labels[i] = p->pending_labels[i];
    }
    p->npending = 0;
}

static bool label_in_use(const cunit *p, embi_js_value name)
{
    for (const ctl *c = p->ctl; c; c = c->parent) {
        for (uint8_t i = 0; i < c->nlabels; i++) {
            if (c->labels[i] == name) {
                return true;
            }
        }
    }
    return false;
}

/* ------------------------------------------------------------------ forward declarations */

static bool parse_expression(cunit *p, bool no_in, expr *e);
static bool parse_assign(cunit *p, bool no_in, expr *e);
static bool parse_statement(cunit *p);
static bool parse_statement_ctx(cunit *p, bool sub);
static bool parse_import_expr(cunit *p, expr *e);
static bool parse_call_member(cunit *p, expr *e, bool allow_call);
static bool is_use_strict_directive_here(const cunit *p);
static bool parse_function_common(cunit *p, embi_js_value name, embi_js_value self_name,
                                  uint32_t src_start, cfunc **out);
static bool parse_call_member_ex(cunit *p, expr *e, bool allow_call, bool keep_ref);
static bool parse_postfix_ex(cunit *p, expr *e, bool keep_ref);
static bool parse_lhs_ex(cunit *p, expr *e, bool keep_ref);
static bool parse_arrow(cunit *p, bool no_in, expr *e);
static bool parse_arrow_async(cunit *p, bool no_in, expr *e, uint32_t start);
static bool parse_function_expr(cunit *p, expr *e, uint32_t start, bool async, embi_js_value hint);
static bool parse_function_decl_kw(cunit *p, bool async);
static bool arrow_ahead_lx(cunit *p, embi_jslex lx, uint32_t paren_start);
static bool parse_param_default(cunit *p, embi_js_value nm);
static bool parse_body(cunit *p, cfunc *f);
static bool parse_lexical_decls(cunit *p, bool is_const, bool no_in, bool for_head,
                                embi_js_value *forin, cbind **forin_bind);

/* ------------------------------------------------------------------ declarations */

/* A var or function declaration may not name a lexical binding that encloses it up to the
 * function's top scope (ES2015 13.2.1, 14.1.1). */
/* The scope that holds a function's vars and top-level functions: its root, or the separate body
 * scope of a function whose parameters have default values. */
static cscope *top_scope(const cfunc *f)
{
    return f->vroot ? f->vroot : f->root;
}

/* Whether the binding x of scope s is lexically declared there: let, const, or a function
 * declared in a block (a function at the top of a function is a var-like binding). */
static bool is_block_lexical(const cfunc *f, const cscope *s, const cbind *x)
{
    return x->kind == BK_LET || x->kind == BK_CONST || x->catch_pat ||
           (x->kind == BK_FUNC && s != top_scope(f));
}

static bool scope_has_var(const cscope *s, embi_js_value name)
{
    for (const cvname *v = s->vars; v; v = v->next) {
        if (v->name == name) {
            return true;
        }
    }
    return false;
}

/* Records that a var of this name is declared inside the blocks around the current scope, so a
 * later let, const or function of the same name in one of them is an early error. */
static void note_var_name(cunit *p, embi_js_value name)
{
    cfunc *f = p->fn;
    cscope *top = top_scope(f);
    for (cscope *s = f->cur; s && s != top; s = s->parent) {
        if (!scope_has_var(s, name)) {
            cvname *v = embi_jsc_alloc(p, sizeof *v);
            v->name = name;
            v->next = s->vars;
            s->vars = v;
        }
    }
}

static bool check_var_conflict(cunit *p, embi_js_value name)
{
    cfunc *f = p->fn;
    cscope *top = top_scope(f);
    for (cscope *s = f->cur; s; s = s->parent) {
        cbind *x = embi_jsc_find_in_scope(s, name);
        if (x && is_block_lexical(f, s, x)) {
            return perr(p, "Identifier has already been declared");
        }
        if (s == top) {
            break;
        }
    }
    return true;
}

/* Annex B.3.3: a function declared in a block also creates a var of its name unless that var
 * would conflict with a lexical declaration of an enclosing block (no early error is raised). */
static bool annexb_var_ok(const cunit *p, const cscope *from, embi_js_value name)
{
    const cfunc *f = p->fn;
    for (const cscope *s = from; s; s = s->parent) {
        const cbind *x = embi_jsc_find_in_scope((cscope *)s, name);
        if (x && is_block_lexical(f, s, x)) {
            return false;
        }
        if (s == top_scope(f)) {
            break;
        }
    }
    return true;
}

/* Declares a let or const binding in the current scope. A name that the scope already binds is
 * an error (a parameter, a var or a function of the top scope included). */
static cbind *declare_lexical(cunit *p, embi_js_value name, uint8_t kind, bool *ok)
{
    cfunc *f = p->fn;
    *ok = true;
    /* A catch parameter is in the scope around the catch block: a let of the same name in that
     * block is an early error (ES2015 13.15.1). */
    bool catch_param = f->cur->parent && f->cur->parent->kind == SC_CATCH &&
                       embi_jsc_find_in_scope(f->cur->parent, name);
    /* A body-level lexical name may not repeat a parameter (separate parameter scope). */
    const cbind *pb = f->vroot && f->cur == f->vroot ? embi_jsc_find_in_scope(f->root, name) : nullptr;
    if (catch_param || (pb && pb->kind == BK_PARAM) || scope_has_var(f->cur, name) ||
        embi_jsc_find_in_scope(f->cur, name)) {
        *ok = perr(p, "Identifier has already been declared");
        return nullptr;
    }
    if (p->collect && f == p->top) {
        embi_vec_push(p->collect, name);
    }
    cbind *b = embi_jsc_new_bind(p, f, f->cur, name, kind);
    b->global = false;
    if (f->is_script && f->cur == f->root) {
        /* Script-level let and const live in the realm's lexical record, shared by all scripts. */
        b->global = true;
        b->glex = true;
    }
    return b;
}

/* Declares a var or function name of the current function. Sloppy eval code declares its names
 * in the caller's variable environment instead, so it creates no binding (nullptr). */
static cbind *declare_var(cunit *p, embi_js_value name, uint8_t kind)
{
    cfunc *f = p->fn;
    if (f->is_eval && !f->strict) {
        for (uint32_t i = 0; i < f->evalvars.len; i++) {
            if (f->evalvars.data[i] == name) {
                return nullptr;
            }
        }
        CF_PUSH(f, evalvars, name);
        return nullptr;
    }
    cscope *top = top_scope(f);
    cbind *b = embi_jsc_find_in_scope(top, name);
    if (b && b->kind == BK_IMPORT) {
        /* J3c: a var or function may not repeat an import (reported after the statement). */
        p->late_err = true;
        return b;
    }
    if (b && f->is_module && f == p->top && (kind == BK_FUNC || b->kind == BK_FUNC) &&
        (b->kind == BK_FUNC || b->kind == BK_VAR)) {
        /* Module code: top-level functions are lexical (ECMA-262 16.2.1.1), so a function may not
         * repeat another function or a var of the same name (reported after the item). */
        p->late_err = true;
    }
    if (p->collect && f == p->top) {
        embi_vec_push(p->collect, name);
    }
    if (b) {
        if (kind == BK_FUNC && b->kind == BK_ARGUMENTS) {
            /* A function declaration named arguments suppresses the arguments object (10.5). */
            b->kind = BK_FUNC;
            f->arguments = nullptr;
        }
        return b;
    }
    if (kind == BK_VAR && name == p->atom_arguments && !embi_jsc_is_top(f) && !f->is_arrow) {
        /* var arguments does not replace the arguments object (10.5 step 7): the name stays
         * bound to the implicit binding, so a later initializer simply assigns to it. */
        return embi_jsc_ensure_arguments(p, f);
    }
    /* With a separate body scope a var of a parameter's name starts as the parameter's value. */
    const cbind *pb = f->vroot ? embi_jsc_find_in_scope(f->root, name) : nullptr;
    b = embi_jsc_new_bind(p, f, top, name, kind);
    b->global = f->is_script;
    if (pb && pb->kind == BK_PARAM && kind == BK_VAR) {
        CF_PUSH(f, pcopies, b);
    }
    return b;
}

static void declare_param(cunit *p, embi_js_value name, uint32_t index)
{
    cfunc *f = p->fn;
    cbind *b = embi_jsc_find_in_scope(f->root, name);
    if (!b) {
        b = embi_jsc_new_bind(p, f, f->root, name, BK_PARAM);
    }
    b->slot = (int32_t)index;
    CF_PUSH(f, pnames, name);
}

/* The strict-mode checks on a function's names and parameters, run once its strictness is known
 * (inherited, or from its directive prologue). */
static bool check_strict_params(cunit *p, cfunc *f)
{
    if (f->self_name != EMBI_JS_EMPTY) {
        if (is_eval_or_args(p, f->self_name)) {
            return perr(p, "Unexpected eval or arguments in strict mode");
        }
        if (is_strict_word(p, f->self_name)) {
            return perr(p, "Unexpected strict mode reserved word");
        }
    }
    for (uint32_t i = 0; i < f->pnames.len; i++) {
        embi_js_value a = f->pnames.data[i];
        if (is_eval_or_args(p, a)) {
            return perr(p, "Unexpected eval or arguments in strict mode");
        }
        if (is_strict_word(p, a)) {
            return perr(p, "Unexpected strict mode reserved word");
        }
        for (uint32_t j = 0; j < i; j++) {
            if (f->pnames.data[j] == a) {
                return perr(p, "Duplicate parameter name not allowed in this context");
            }
        }
    }
    return true;
}

/* Whether new.target is valid here: inside a function other than an arrow function (or an arrow
 * function inside one), or in eval code whose caller was inside one. */
static bool new_target_allowed(const cunit *p)
{
    for (const cfunc *g = p->fn; g; g = g->parent) {
        if (!g->is_arrow) {
            return !embi_jsc_is_top(g) || (g->is_eval && p->nt_ok);
        }
    }
    return false;
}

static bool super_prop_ok(cunit *p);
static cfunc *nonarrow(cunit *p);

/* A direct eval call. Its scope entries are resolved later. The function gets an arguments
 * object, and when sloppy a variable environment for the vars that the eval declares. */
static uint32_t new_eval_site(cunit *p, uint32_t argc, uint32_t name_k)
{
    cfunc *f = p->fn;
    f->has_eval = true;
    if (!embi_jsc_is_top(f)) {
        if (!f->strict && !f->ve_bind) {
            f->ve_bind = embi_jsc_new_bind(p, f, f->root, tmp_name(p, 'e'), BK_TEMP);
        }
        if (f->is_arrow) {
            /* The eval code sees the arguments object of the nearest enclosing function. */
            cfunc *g = f;
            while (g->is_arrow) {
                g = g->parent;
            }
            if (!embi_jsc_is_top(g) && !embi_jsc_find_in_scope(g->root, p->atom_arguments)) {
                (void)embi_jsc_ensure_arguments(p, g);
            }
        } else if (!embi_jsc_find_in_scope(f->root, p->atom_arguments)) {
            f->arguments = embi_jsc_new_bind(p, f, f->root, p->atom_arguments, BK_ARGUMENTS);
        }
    }
    ceval ce;
    memset(&ce, 0, sizeof ce);
    ce.this_up = -1;
    ce.nt_up = -1;
    ce.home_up = -1;
    ce.super_ok = super_prop_ok(p);
    ce.priv_ok = p->cls != nullptr || p->eval_priv;
    {
        cfunc *ng = nonarrow(p);
        ce.noargs = ng && (ng->is_field_init || ng->is_static_block || (ng->is_eval && p->eval_noargs));
    }
    ce.nt_ok = new_target_allowed(p);
    ce.in_params = f->in_params;
    ce.args_bad = f->in_params;
    ce.scope = f->cur;
    ce.argc = argc;
    ce.name_k = name_k;
    ce.strict = f->strict;
    ce.first = 0;
    ce.n = 0;
    {
        embi_js_value mh = p->eval_mode ? p->eval_module : (p->mod ? p->holder : EMBI_JS_EMPTY);
        ce.mod_k = (mh != EMBI_JS_EMPTY && embi_js_is_object(mh)) ? const_str(p, mh) + 1u : 0u;
    }
    uint32_t idx = (uint32_t)f->evals.len;
    CF_PUSH(f, evals, ce);
    return idx;
}

/* ------------------------------------------------------------------ expressions */

static bool tok_is_word(const cunit *p, const char *w)
{
    size_t n = strlen(w);
    return !(p->tok.flags & EMBI_JST_F_ESCAPED) && p->tok.end - p->tok.start == n &&
           memcmp(p->src + p->tok.start, w, n) == 0;
}

static bool arrow_ahead(cunit *p)
{
    return arrow_ahead_lx(p, p->lx, p->tok.start);
}

/* Whether the current '(' starts an arrow function's parameter list: the matching ')' is followed
 * by '=>' on the same line. Scans a copy of the lexer, so the parse position does not move. A '/'
 * is read as a regular expression when no operand can precede it. Any error or an unbalanced
 * bracket answers false, and the parenthesised expression then reports its own error. (FP1a: a
 * malformed parameter list therefore gets the error message of the parenthesised expression, not
 * a parameter-list one; the change is intentional.) */
static bool arrow_ahead_lx(cunit *p, embi_jslex lx, uint32_t paren_start)
{
    /* FP1a: the answer is a pure function of the '(' offset, and the scan passes every '(' nested in
     * it, so the answers for those (the ones that can start a primary expression) are memoized as
     * the scan closes them: the parser's later lookups at the inner parentheses are table hits and
     * the work over nested parentheses stays linear. */
    uint64_t memo;
    if (embi_hashmap_get(&p->arr, paren_start, &memo)) {
        return memo != 0;
    }
    lx.skim = true;
    lx.scratch = p->peek_scratch;
    lx.scratch_cap = p->peek_cap;
    embi_jstok t;
    uint8_t open[256]; /* '(' '[' '{', or 'T' for an open template substitution */
    uint32_t starts[sizeof open]; /* offset of each open primary '(' (UINT32_MAX: not memoized) */
    uint32_t sp = 1;
    open[0] = '(';
    starts[0] = paren_start;
    uint32_t pend = UINT32_MAX; /* a '(' just closed: the next token decides its answer */
    bool operand = false;
    /* The shape of the list at its own level: 0 expects a parameter, 1 follows one (a name or a
     * closed pattern), 2 is inside a default value, where anything goes. A token that no parameter
     * list can hold there (the "function" of an IIFE, a number, an operator) ends the scan with no:
     * the parenthesised expression is then parsed as such, and "(a + b) => c" reports its error
     * there. This keeps a big parenthesised function from being scanned to its end. */
    uint8_t pstate = 0;
    for (;;) {
        bool before = operand;
        lx.regex_ok = !operand;
        lex_scan(p, &lx, &t, true);
        if (sp == 1 && pend == UINT32_MAX && pstate != 2) {
            bool ok;
            switch (t.kind) {
            case EMBI_JST_NAME:
                ok = pstate == 0 && !reserved_kw(t.kw);
                pstate = 1;
                break;
            case EMBI_JST_ELLIPSIS:
                ok = pstate == 0;
                break;
            case EMBI_JST_LBRACK:
            case EMBI_JST_LBRACE:
                ok = pstate == 0;
                break;
            case EMBI_JST_COMMA:
                ok = pstate == 1;
                pstate = 0;
                break;
            case EMBI_JST_ASSIGN:
                ok = pstate == 1;
                pstate = 2;
                break;
            case EMBI_JST_RPAREN:
                ok = true;
                break;
            default:
                ok = false;
                break;
            }
            if (!ok) {
                (void)embi_hashmap_put(&p->arr, paren_start, 0u);
                return false;
            }
        }
        if (sp == 1 && pstate == 2 && t.kind == EMBI_JST_COMMA) {
            pstate = 0;
        }
        if (pend != UINT32_MAX) {
            bool arrow = t.kind == EMBI_JST_ARROW && !(t.flags & EMBI_JST_F_NL_BEFORE);
            (void)embi_hashmap_put(&p->arr, pend, arrow ? 1u : 0u);
            if (pend == paren_start) {
                return arrow;
            }
            pend = UINT32_MAX;
        }
        switch (t.kind) {
        case EMBI_JST_EOF:
        case EMBI_JST_ERROR:
            return false;
        case EMBI_JST_LPAREN:
        case EMBI_JST_LBRACK:
        case EMBI_JST_LBRACE:
            if (sp == sizeof open) {
                return false;
            }
            starts[sp] = t.kind == EMBI_JST_LPAREN && !before ? t.start : UINT32_MAX;
            open[sp++] = t.kind == EMBI_JST_LPAREN ? '(' : t.kind == EMBI_JST_LBRACK ? '[' : '{';
            operand = false;
            break;
        case EMBI_JST_TPL_HEAD:
            if (sp == sizeof open) {
                return false;
            }
            starts[sp] = UINT32_MAX;
            open[sp++] = 'T';
            operand = false;
            break;
        case EMBI_JST_RPAREN:
        case EMBI_JST_RBRACK:
            if (sp == 0 || open[sp - 1] != (t.kind == EMBI_JST_RPAREN ? '(' : '[')) {
                return false;
            }
            sp--;
            pend = starts[sp];
            operand = true;
            if (sp == 1 && pstate != 2) {
                pstate = 1;
            }
            break;
        case EMBI_JST_RBRACE:
            if (sp <= 1) {
                return false;
            }
            if (open[sp - 1] == 'T') {
                embi_jslex_rescan_template(&lx, &t);
                if (t.kind == EMBI_JST_TPL_MIDDLE) {
                    operand = false;
                } else if (t.kind == EMBI_JST_TPL_TAIL) {
                    sp--;
                    operand = true;
                } else {
                    return false;
                }
                break;
            }
            if (open[sp - 1] != '{') {
                return false;
            }
            sp--;
            operand = true;
            if (sp == 1 && pstate != 2) {
                pstate = 1;
            }
            break;
        case EMBI_JST_NAME:
            operand = t.kw == EMBI_JSKW_NONE || t.kw == EMBI_JSKW_this || t.kw == EMBI_JSKW_null ||
                      t.kw == EMBI_JSKW_true || t.kw == EMBI_JSKW_false;
            break;
        case EMBI_JST_NUMBER:
        case EMBI_JST_STRING:
        case EMBI_JST_TPL_NOSUB:
        case EMBI_JST_TPL_TAIL:
        case EMBI_JST_REGEX:
        case EMBI_JST_BIGINT:
        case EMBI_JST_INC:
        case EMBI_JST_DEC:
            operand = true;
            break;
        default:
            operand = false;
            break;
        }
    }
}

/* One template part at the current token (TPL_NOSUB, TPL_HEAD, TPL_MIDDLE or TPL_TAIL): its cooked
 * and raw strings, as constants. An invalid escape is an error in an untagged template and an
 * undefined cooked value in a tagged one. */
static bool template_part(cunit *p, bool tagged, uint32_t *cooked_k, uint32_t *raw_k)
{
    if (p->tok.flags & EMBI_JST_F_BAD_ESCAPE) {
        if (!tagged) {
            return perr(p, "Invalid escape sequence in template");
        }
        *cooked_k = embi_jsc_add_const(p, EMBI_JS_UNDEFINED);
    } else {
        embi_js_value v =
            embi_js_from_string(embi_js_str_new_utf16(p->rt, p->tok.cooked, p->tok.cooked_len));
        *cooked_k = const_str(p, v);
    }
    if (raw_k == nullptr) {
        return true; /* FP2a: the raw text is only the template object's (tagged templates) */
    }
    uint32_t n = embi_jslex_raw16(&p->lx, &p->tok, nullptr, 0);
    size_t bytes = (size_t)n * sizeof(uint16_t) + 2u;
    uint16_t *buf = embi_alloc(bytes);
    (void)embi_jslex_raw16(&p->lx, &p->tok, buf, n);
    embi_js_value rv = embi_js_from_string(embi_js_str_new_utf16(p->rt, buf, n));
    embi_free(buf, bytes);
    *raw_k = const_str(p, rv);
    return true;
}

/* An untagged template literal: the parts and the ToString of each substitution are concatenated.
 * The current token is TPL_NOSUB or TPL_HEAD. */
static bool parse_template(cunit *p, expr *e)
{
    uint32_t ck = 0;
    if (!template_part(p, false, &ck, nullptr)) {
        return false;
    }
    OPC32(PUSH_CONST, ck);
    if (is(p, EMBI_JST_TPL_NOSUB)) {
        adv(p);
        set_value(e);
        return true;
    }
    for (;;) {
        adv(p);
        expr x;
        if (!parse_expression(p, false, &x)) {
            return false;
        }
        to_value(p, &x);
        OPC(TO_STRING);
        OPC(ADD);
        if (!is(p, EMBI_JST_RBRACE)) {
            return unexpected(p);
        }
        embi_jslex_rescan_template(&p->lx, &p->tok);
        if (!is(p, EMBI_JST_TPL_MIDDLE) && !is(p, EMBI_JST_TPL_TAIL)) {
            return unexpected(p);
        }
        if (!template_part(p, false, &ck, nullptr)) {
            return false;
        }
        OPC32(PUSH_CONST, ck);
        OPC(ADD);
        if (is(p, EMBI_JST_TPL_TAIL)) {
            adv(p);
            break;
        }
    }
    set_value(e);
    return true;
}

/* The arguments of a tagged template after the callee pair. The template object is a constant
 * of this site, built once here (frozen, with a frozen raw array), so every evaluation of the site
 * passes the same object. Its constant slot is reserved first and filled once the literal is read.
 * Sets argc to the number of arguments (the object and the substitutions). */
static bool parse_tagged_template(cunit *p, uint32_t *argc)
{
    EMBI_VEC(uint32_t) cooked = {};
    EMBI_VEC(uint32_t) raw = {};
    bool ok = false;
    uint32_t tk = embi_jsc_add_const(p, EMBI_JS_UNDEFINED);
    OPC32(PUSH_CONST, tk);
    uint32_t n = 0;
    uint32_t ck = 0;
    uint32_t rk = 0;
    if (!template_part(p, true, &ck, &rk)) {
        goto out;
    }
    embi_vec_push(&cooked, ck);
    embi_vec_push(&raw, rk);
    if (is(p, EMBI_JST_TPL_NOSUB)) {
        adv(p);
    } else {
        for (;;) {
            adv(p);
            expr x;
            if (!parse_expression(p, false, &x)) {
                goto out;
            }
            to_value(p, &x);
            n++;
            if (!is(p, EMBI_JST_RBRACE)) {
                (void)unexpected(p);
                goto out;
            }
            embi_jslex_rescan_template(&p->lx, &p->tok);
            if (!is(p, EMBI_JST_TPL_MIDDLE) && !is(p, EMBI_JST_TPL_TAIL)) {
                (void)unexpected(p);
                goto out;
            }
            if (!template_part(p, true, &ck, &rk)) {
                goto out;
            }
            embi_vec_push(&cooked, ck);
            embi_vec_push(&raw, rk);
            if (is(p, EMBI_JST_TPL_TAIL)) {
                adv(p);
                break;
            }
        }
    }
    {
        embi_js_object *co = embi_js_array_create_default(p->rt, 0);
        embi_js_object *ro = embi_js_array_create_default(p->rt, 0);
        if (!co || !ro) {
            goto out;
        }
        for (uint32_t i = 0; i < cooked.len; i++) {
            embi_js_key k = embi_js_key_from_index(i);
            if (embi_js_create_data_property(p->rt, co, k, p->fn->consts.data[cooked.data[i]]) ==
                    EMBI_JS_THREW ||
                embi_js_create_data_property(p->rt, ro, k, p->fn->consts.data[raw.data[i]]) ==
                    EMBI_JS_THREW) {
                goto out;
            }
        }
        embi_js_value raw_key = embi_js_from_string(embi_js_atom_intern_utf8(p->rt, "raw", 3));
        embi_js_define_raw(p->rt, co, raw_key, embi_js_from_object(ro), 0);
        if (embi_js_set_integrity(p->rt, ro, true) == EMBI_JS_THREW ||
            embi_js_set_integrity(p->rt, co, true) == EMBI_JS_THREW) {
            goto out;
        }
        p->fn->consts.data[tk] = embi_js_from_object(co);
    }
    *argc = n + 1u;
    ok = true;
out:
    embi_vec_free(&cooked);
    embi_vec_free(&raw);
    return ok;
}

/* The callee pair of a call: a name is called with undefined as its this value, a member with its
 * base, and any other value with undefined. */
/* A parenthesized optional chain used as a call target, (a?.b)(): the member reference kept its
 * receiver. The chain's short-circuit jumps arrive with undefined as the callee, so they get an
 * undefined this value before they join the normal path. */
static void callee_opt_join(cunit *p, expr *e)
{
    if (!e->opt) {
        return;
    }
    int32_t depth = p->fn->depth;
    uint32_t over = jump_emit(p, EMBI_JSOP_JMP);
    embi_jsc_patch_list(p, e->opt, embi_jsc_here(p));
    e->opt = nullptr;
    p->fn->depth = depth - 1;
    OPC(PUSH_UNDEF);
    jump_to(p, over, embi_jsc_here(p));
}

static void emit_callee(cunit *p, expr *e)
{
    switch (e->kind) {
    case EX_VAR:
        mark(p, e->line, e->col);
        embi_jsc_emit_sym(p, SYM_KIND_CALL, e->name, e->scope, nullptr);
        set_value(e);
        OPC(PUSH_UNDEF);
        break;
    case EX_FIELD:
        mark(p, e->line, e->col);
        OPC32(GET_FIELD_THIS, e->key);
        callee_opt_join(p, e);
        break;
    case EX_INDEX:
        mark(p, e->line, e->col);
        OPC(GET_INDEX_THIS);
        callee_opt_join(p, e);
        break;
    case EX_SUPER_FIELD:
        OPC32(SUPER_GET_THIS, e->key);
        break;
    case EX_SUPER_INDEX:
        OPC(SUPER_GET_THIS_INDEX);
        break;
    case EX_PRIV:
        mark(p, e->line, e->col);
        OPC(PRIV_GET_THIS);
        break;
    default:
        OPC(PUSH_UNDEF);
        break;
    }
}

/* ||=, &&= and ??=. The reference is read; the right-hand side is evaluated and stored only when
 * the operator does not short-circuit. A short-circuit leaves the value as the result (the base
 * and key of a member reference are dropped). An anonymous function on the right takes the name of
 * an identifier target. */
static bool logical_assign(cunit *p, const expr *lhs, uint8_t kind, bool no_in)
{
    uint8_t jop = kind == EMBI_JST_LAND_EQ   ? EMBI_JSOP_JMP_FALSE_KEEP
                  : kind == EMBI_JST_LOR_EQ  ? EMBI_JSOP_JMP_TRUE_KEEP
                                             : EMBI_JSOP_JMP_NOT_NULLISH_KEEP;
    ref_dup(p, lhs);
    ref_get(p, lhs);
    uint32_t j = jump_emit(p, jop);
    int32_t d_short = p->fn->depth;
    p->fn->depth -= 1; /* the fall-through pops the value */
    if (lhs->kind == EX_VAR && !lhs->paren) {
        p->hint = lhs->name;
    }
    expr rhs;
    if (!parse_assign(p, no_in, &rhs)) {
        return false;
    }
    to_value(p, &rhs);
    ref_set(p, lhs);
    uint32_t je = jump_emit(p, EMBI_JSOP_JMP);
    int32_t d_end = p->fn->depth;
    jump_to(p, j, embi_jsc_here(p));
    p->fn->depth = d_short;
    if (lhs->kind == EX_FIELD) {
        OPC(SWAP);
        OPC(POP);
    } else if (lhs->kind == EX_INDEX || lhs->kind == EX_PRIV || lhs->kind == EX_SUPER_FIELD) {
        OPC(ROT3);
        OPC(POP);
        OPC(POP);
    } else if (lhs->kind == EX_SUPER_INDEX) {
        OPC(ROT4);
        OPC(POP);
        OPC(POP);
        OPC(POP);
    }
    jump_to(p, je, embi_jsc_here(p));
    p->fn->depth = d_end;
    return true;
}

static bool parse_arguments(cunit *p, uint32_t *argc, bool *spread)
{
    /* at '(' */
    adv(p);
    uint32_t n = 0;        /* plain arguments on the stack */
    bool as_array = false; /* a spread argument was seen: the list is one array from here on */
    if (!is(p, EMBI_JST_RPAREN)) {
        for (;;) {
            bool sp = is(p, EMBI_JST_ELLIPSIS);
            if (sp) {
                adv(p);
                if (!as_array) {
                    embi_jsc_emit_op_eff(p, EMBI_JSOP_ARRAY_FROM_STACK, n, (int)n, 1);
                    as_array = true;
                }
            }
            expr a;
            if (!parse_assign(p, false, &a)) {
                return false;
            }
            to_value(p, &a);
            if (as_array) {
                embi_jsc_emit_op(p, sp ? EMBI_JSOP_ARRAY_SPREAD : EMBI_JSOP_ARRAY_PUSH);
            } else {
                n++;
            }
            if (is(p, EMBI_JST_COMMA)) {
                adv(p);
                if (is(p, EMBI_JST_RPAREN)) {
                    break;
                }
                continue;
            }
            break;
        }
    }
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    if (!as_array && n > 0xFFFFu) {
        return perr(p, "Too many arguments");
    }
    *argc = as_array ? 0 : n;
    *spread = as_array;
    return true;
}

/* An array literal. Plain elements are defined by index while the array is built in place. The
 * first spread switches to appending (ARRAY_PUSH, ARRAY_HOLE, ARRAY_SPREAD), after the holes that
 * precede it are made. */
static bool parse_array_literal(cunit *p, expr *e)
{
    adv(p); /* [ */
    uint32_t pc = OPC32(NEW_ARRAY, 0);
    uint32_t idx = 0;     /* index of the next element */
    uint32_t defined = 0; /* one past the last element defined by index */
    bool dynamic = false; /* elements are appended */
    while (!is(p, EMBI_JST_RBRACK)) {
        if (is(p, EMBI_JST_COMMA)) {
            if (dynamic) {
                OPC(ARRAY_HOLE);
            }
            idx++;
            adv(p);
            continue;
        }
        bool sp = is(p, EMBI_JST_ELLIPSIS);
        if (sp) {
            if (!dynamic) {
                for (uint32_t i = defined; i < idx; i++) {
                    OPC(ARRAY_HOLE);
                }
                dynamic = true;
            }
            adv(p);
        }
        expr v;
        if (!parse_assign(p, false, &v)) {
            return false;
        }
        to_value(p, &v);
        if (sp) {
            OPC(ARRAY_SPREAD);
        } else if (dynamic) {
            OPC(ARRAY_PUSH);
        } else {
            OPC32(DEFINE_INDEX, idx);
            defined = idx + 1u;
        }
        idx++;
        if (is(p, EMBI_JST_COMMA)) {
            adv(p);
        } else if (!is(p, EMBI_JST_RBRACK)) {
            return unexpected(p);
        }
    }
    adv(p); /* ] */
    if (!dynamic) {
        embi_js_wr32(&p->fn->code.data[pc + 1], idx);
    }
    set_value(e);
    e->literal = 1;
    return true;
}


/* Property names of object literals: IdentifierName, string, or number. */
static bool property_key(cunit *p, embi_js_value *out)
{
    switch (p->tok.kind) {
    case EMBI_JST_NAME:
        *out = name_atom(p);
        return true;
    case EMBI_JST_STRING: {
        if ((p->tok.flags & EMBI_JST_F_LEGACY) && p->fn->strict) {
            return perr(p, "Octal escape sequences are not allowed in strict mode");
        }
        embi_js_string *s = embi_js_str_new_utf16(p->rt, p->tok.cooked, p->tok.cooked_len);
        *out = embi_js_from_string(embi_js_atom_intern(p->rt, s));
        return true;
    }
    case EMBI_JST_NUMBER: {
        if ((p->tok.flags & EMBI_JST_F_LEGACY) && p->fn->strict) {
            return perr(p, "Octal literals and decimals with leading zeros are not allowed in strict mode");
        }
        embi_js_string *s = embi_js_number_to_str(p->rt, p->tok.num);
        *out = embi_js_from_string(embi_js_atom_intern(p->rt, s));
        return true;
    }
    case EMBI_JST_BIGINT: {
        /* A BigInt key is the decimal text of its value (J4b). */
        embi_js_value v = bigint_token_value(p);
        if (embi_js_is_exception(v)) {
            return false;
        }
        embi_js_string *s = embi_js_bigint_to_string(p->rt, embi_js_as_bigint(v), 10);
        if (!s) {
            return false;
        }
        *out = embi_js_from_string(embi_js_atom_intern(p->rt, s));
        return true;
    }
    default:
        return unexpected(p);
    }
}

/* An object literal (ES2015 11.1.5). Property definitions run in source order. Computed keys are
 * converted with ToPropertyKey once, before the value is evaluated. A function value takes the
 * name of its key: at compile time for a static key, at run time (SET_FN_NAME) for a computed one.
 * Duplicate names are allowed, except two __proto__: value forms. */

static bool parse_object_literal(cunit *p, expr *e)
{
    adv(p); /* { */
    OPC(NEW_OBJECT);
    bool proto_seen = false;
    while (!is(p, EMBI_JST_RBRACE)) {
        uint32_t item_start = p->tok.start;
        if (is(p, EMBI_JST_ELLIPSIS)) {
            adv(p);
            expr src;
            if (!parse_assign(p, false, &src)) {
                return false;
            }
            to_value(p, &src);
            OPC(COPY_DATA_PROPS);
        } else {
            bool async_method = false;
            if (is_kw(p, EMBI_JSKW_async) && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
                embi_jstok nt;
                peek(p, &nt);
                if (!(nt.flags & EMBI_JST_F_NL_BEFORE) &&
                    (nt.kind == EMBI_JST_NAME || nt.kind == EMBI_JST_STRING ||
                     nt.kind == EMBI_JST_NUMBER || nt.kind == EMBI_JST_BIGINT ||
                     nt.kind == EMBI_JST_LBRACK || nt.kind == EMBI_JST_MUL ||
                     nt.kind == EMBI_JST_PRIVATE_NAME)) {
                    async_method = true;
                    adv(p);
                }
            }
            bool gen_method = false;
            if (is(p, EMBI_JST_MUL)) {
                gen_method = true;
                adv(p);
            }
            bool getter = false;
            bool setter = false;
            if (is_kw(p, EMBI_JSKW_get) || is_kw(p, EMBI_JSKW_set)) {
                embi_jstok nt;
                peek(p, &nt);
                bool accessor = nt.kind != EMBI_JST_COLON && nt.kind != EMBI_JST_COMMA &&
                                nt.kind != EMBI_JST_LPAREN && nt.kind != EMBI_JST_RBRACE;
                if (accessor && async_method) {
                    return perr(p, "Unexpected token in an async method");
                }
                if (accessor) {
                    getter = p->tok.kw == EMBI_JSKW_get;
                    setter = !getter;
                    adv(p);
                }
            }
            bool computed = is(p, EMBI_JST_LBRACK);
            bool ident_key = !computed && is_ident(p);
            embi_js_value key = EMBI_JS_EMPTY;
            uint32_t kc = 0;
            uint32_t line = p->tok.line;
            uint32_t col = p->tok.col;
            if (computed) {
                adv(p);
                expr k;
                if (!parse_assign(p, false, &k)) {
                    return false;
                }
                to_value(p, &k);
                if (!expect(p, EMBI_JST_RBRACK)) {
                    return false;
                }
                OPC(TO_PROPKEY);
            } else {
                if (!property_key(p, &key)) {
                    return false;
                }
                adv(p);
                kc = const_str(p, key);
            }
            /* The async prefix is only a method's: `({async async})` and `({async a})` are errors. */
            if (async_method && !is(p, EMBI_JST_LPAREN)) {
                return perr(p, "Unexpected token in an async method");
            }
            if (gen_method && !getter && !setter && !is(p, EMBI_JST_LPAREN)) {
                return perr(p, "Unexpected token after a generator method name");
            }
            if (getter || setter) {
                if (gen_method) {
                    return perr(p, "Unexpected token in a generator accessor");
                }
                if (!computed) {
                    OPC32(PUSH_CONST, kc);
                }
                cfunc *f;
                p->next_kind = getter ? 2 : 3;
                if (!parse_function_common(p, EMBI_JS_EMPTY, EMBI_JS_EMPTY, item_start, &f)) {
                    return false;
                }
                if (getter ? f->nparams != 0 : f->nparams != 1) {
                    return perr(p, getter ? "Getter must not have any formal parameters"
                                          : "Setter must have exactly one formal parameter");
                }
                f->new_pc = embi_jsc_here(p);
                OPC32(NEW_CLOSURE, f->const_idx);
                OPC32(SET_HOME, 1); /* [obj key fn] */
                OPC32(SET_FN_NAME, getter ? 1u : 2u);
                mark(p, line, col);
                embi_jsc_emit_op(p, getter ? EMBI_JSOP_DEFINE_GETTER_COMPUTED
                                           : EMBI_JSOP_DEFINE_SETTER_COMPUTED);
            } else if (is(p, EMBI_JST_LPAREN)) {
                cfunc *f;
                embi_js_value fname = computed ? EMBI_JS_EMPTY : key;
                p->next_kind = 1;
                p->next_gen = gen_method;
                p->next_async = async_method;
                if (!parse_function_common(p, fname, EMBI_JS_EMPTY, item_start, &f)) {
                    return false;
                }
                f->new_pc = embi_jsc_here(p);
                OPC32(NEW_CLOSURE, f->const_idx);
                OPC32(SET_HOME, computed ? 1u : 0u); /* [obj key fn] or [obj fn] */
                if (computed) {
                    OPC32(SET_FN_NAME, 0);
                    OPC(DEFINE_COMPUTED);
                } else {
                    OPC32(DEFINE_FIELD, kc);
                }
            } else if (is(p, EMBI_JST_COLON)) {
                adv(p);
                if (!computed && key == p->atom_proto) {
                    if (proto_seen) {
                        return perr(p, "Duplicate __proto__ fields are not allowed in object literals");
                    }
                    proto_seen = true;
                    expr v;
                    if (!parse_assign(p, false, &v)) {
                        return false;
                    }
                    to_value(p, &v);
                    OPC(SET_PROTO_LIT);
                } else if (computed) {
                    expr v;
                    if (!parse_assign(p, false, &v)) {
                        return false;
                    }
                    to_value(p, &v);
                    if (v.anon) {
                        OPC32(SET_FN_NAME, 0);
                    }
                    OPC(DEFINE_COMPUTED);
                } else {
                    p->hint = key;
                    expr v;
                    if (!parse_assign(p, false, &v)) {
                        return false;
                    }
                    to_value(p, &v);
                    OPC32(DEFINE_FIELD, kc);
                }
            } else if (ident_key && (is(p, EMBI_JST_COMMA) || is(p, EMBI_JST_RBRACE))) {
                /* shorthand: { name } reads the binding of that name */
                mark(p, line, col);
                embi_jsc_emit_sym(p, SYM_KIND_GET, key, p->fn->cur, nullptr);
                OPC32(DEFINE_FIELD, kc);
            } else {
                return unexpected(p);
            }
        }
        if (is(p, EMBI_JST_COMMA)) {
            adv(p);
        } else if (!is(p, EMBI_JST_RBRACE)) {
            return unexpected(p);
        }
    }
    adv(p); /* } */
    set_value(e);
    return true;
}


/* ------------------------------------------------------------------ classes (J2d)
 *
 * A class definition is emitted into the enclosing function (the owner). Its parts:
 *   - the class scope: the inner name, the private names (#x bindings holding private symbols),
 *     and temporaries (the constructor F, the prototype P, computed keys, private methods and
 *     accessors), all named bindings of the class scope so that any function of the class reaches
 *     them by name;
 *   - the constructor F: a child function created at the class start (NEW_CLOSURE + CLASS_SETUP
 *     link it to P); its body is parsed in place when the constructor member is seen (or emitted
 *     as an empty body);
 *   - %init: a child function holding the instance field definitions (run by INIT_FIELDS, which
 *     the interpreter calls at construction, or after super() returns in a derived constructor);
 *     %pinit installs the private methods and accessors first;
 *   - %sinit: a child function holding the static fields and static blocks, run once at the end.
 * The class start jumps to a tail that creates the private names (they must exist before any
 * computed key runs, and the body is only known after it is parsed), then jumps back. */

enum { CP_FIELD, CP_METHOD, CP_ACCESSOR };

/* A private name declared by the class body. */
typedef struct cpriv {
    embi_js_value name; /* "#x" */
    uint8_t kind;       /* CP_* */
    bool stat;
    bool has_get;
    bool has_set;
    cbind *bind;        /* the name's binding (holds the private symbol) */
    cbind *tm;          /* method: the method closure */
    cbind *tg;          /* accessor: the getter (undefined when absent) */
    cbind *ts;          /* accessor: the setter (undefined when absent) */
} cpriv;

/* A reference to a private name, checked when its class ends. */
typedef struct cpref {
    embi_js_value name;
    uint32_t line;
    uint32_t col;
} cpref;

typedef struct cclass cclass;

struct cclass {
    struct cclass *outer;
    cfunc *owner;   /* the function that runs the class definition */
    cscope *scope;  /* the class scope */
    cbind *name_b;  /* the inner name binding, or nullptr for an anonymous class */
    cbind *tF;      /* temporary: the constructor */
    cbind *tP;      /* temporary: the prototype */
    cfunc *ctor;
    cfunc *init;    /* instance field initializer, or nullptr */
    cfunc *sinit;   /* static fields and blocks, or nullptr */
    bool derived;
    bool has_ctor;
    EMBI_VEC(cpriv) privs;
    EMBI_VEC(cpref) refs;
};

static embi_js_value catom(cunit *p, const char *s)
{
    return embi_js_from_string(embi_js_atom_intern_utf8(p->rt, s, strlen(s)));
}

/* An atom spelled by the source text [start, end), with an optional prefix. */
static embi_js_value text_atom(cunit *p, const char *pre, uint32_t start, uint32_t end)
{
    size_t pn = strlen(pre);
    size_t n = pn + (end - start);
    char *buf = embi_jsc_alloc(p, n + 1u);
    memcpy(buf, pre, pn);
    memcpy(buf + pn, p->src + start, end - start);
    buf[n] = '\0';
    return embi_js_from_string(embi_js_atom_intern_utf8(p->rt, buf, n));
}

/* The private name of the current PRIVATE_NAME token (its spelling includes the '#'). */
static embi_js_value priv_atom_pre(cunit *p, const char *pre)
{
    if (p->tok.flags & EMBI_JST_F_ESCAPED) {
        /* pre + "#" + the escape-decoded name, so #\u{6F} and #o are the same private name. */
        size_t pn = strlen(pre);
        size_t n = pn + 1u + p->tok.cooked_len;
        uint16_t *buf = embi_jsc_alloc(p, n * sizeof *buf);
        for (size_t i = 0; i < pn; i++) {
            buf[i] = (uint16_t)(uint8_t)pre[i];
        }
        buf[pn] = u'#';
        memcpy(buf + pn + 1u, p->tok.cooked, (size_t)p->tok.cooked_len * sizeof *buf);
        embi_js_string *s = embi_js_str_new_utf16(p->rt, buf, (uint32_t)n);
        return embi_js_from_string(embi_js_atom_intern(p->rt, s));
    }
    return text_atom(p, pre, p->tok.start, p->tok.end);
}

static embi_js_value priv_atom(cunit *p)
{
    return priv_atom_pre(p, "");
}

/* The nearest function that is not an arrow function (the one that binds this and super). */
static cfunc *nonarrow(cunit *p)
{
    cfunc *g = p->fn;
    while (g && g->is_arrow) {
        g = g->parent;
    }
    return g;
}

static bool this_is_derived(cunit *p)
{
    cfunc *g = nonarrow(p);
    return g && g->is_derived;
}

/* this (J2d): the frame's this in ordinary functions; the this binding in arrows and in derived
 * constructors, where it starts uninitialized and super() sets it. */
static void emit_this(cunit *p)
{
    bool derived = this_is_derived(p);
    if (p->fn->is_arrow || derived) {
        embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_this, p->fn->cur, nullptr);
        if (derived) {
            OPC(CHECK_THIS);
        }
    } else {
        OPC(PUSH_THIS);
    }
}

/* The this binding read without the initialization check (derived constructor returns). */
static void emit_this_raw(cunit *p)
{
    embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_this, p->fn->cur, nullptr);
}

/* [res] -> [res]: binds this to the result of super() (once), then runs the instance fields. */
static void super_bind(cunit *p)
{
    emit_this_raw(p);
    OPC(THIS_UNINIT);
    embi_jsc_emit_sym(p, SYM_KIND_SET, p->atom_this, p->fn->cur, nullptr);
    OPC(DUP);
    embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_fn, p->fn->cur, nullptr);
    OPC(SWAP);
    OPC(INIT_FIELDS);
}

/* Ends a function body: the implicit return undefined, or in a derived constructor the this value
 * (CTOR_RETURN checks that it is initialized). */
static void emit_implicit_return(cunit *p)
{
    OPC(PUSH_UNDEF);
    emit_fn_return(p);
}

static bool super_call_ok(cunit *p)
{
    cfunc *g = nonarrow(p);
    return g && g->is_derived;
}

static bool super_prop_ok(cunit *p)
{
    cfunc *g = nonarrow(p);
    if (g && g->is_eval) {
        return p->eval_super;
    }
    return g && (g->is_method || g->is_field_init || g->is_static_block);
}

/* The super keyword (J2d): a super call, or the start of a super property reference, whose base
 * [this home] is pushed here. The member name is read here too (super.x, super[x]). */
static bool parse_super(cunit *p, expr *e)
{
    uint32_t line = p->tok.line;
    uint32_t col = p->tok.col;
    adv(p); /* super */
    if (is(p, EMBI_JST_LPAREN)) {
        if (!super_call_ok(p)) {
            return perr(p, "'super' keyword unexpected here");
        }
        embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_fn, p->fn->cur, nullptr);
        embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_nt, p->fn->cur, nullptr);
        uint32_t argc = 0;
        bool spread = false;
        if (!parse_arguments(p, &argc, &spread)) {
            return false;
        }
        mark(p, line, col);
        if (spread) {
            embi_jsc_emit_op_eff(p, EMBI_JSOP_SUPER_CALL_SPREAD, 0, 3, 1);
        } else {
            embi_jsc_emit_op_eff(p, EMBI_JSOP_SUPER_CALL, argc, (int)argc + 2, 1);
        }
        super_bind(p);
        set_value(e);
        return true;
    }
    if (!(is(p, EMBI_JST_DOT) || is(p, EMBI_JST_LBRACK)) || !super_prop_ok(p)) {
        return perr(p, "'super' keyword unexpected here");
    }
    emit_this(p);
    embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_home, p->fn->cur, nullptr);
    set_value(e);
    e->line = line;
    e->col = col;
    if (is(p, EMBI_JST_DOT)) {
        adv(p);
        if (p->tok.kind != EMBI_JST_NAME) {
            return unexpected(p);
        }
        embi_js_value key = name_atom(p);
        adv(p);
        e->kind = EX_SUPER_FIELD;
        e->name = key;
        e->key = const_str(p, key);
        return true;
    }
    adv(p); /* [ */
    expr idx;
    if (!parse_expression(p, false, &idx)) {
        return false;
    }
    to_value(p, &idx);
    if (!expect(p, EMBI_JST_RBRACK)) {
        return false;
    }
    e->kind = EX_SUPER_INDEX;
    return true;
}

/* A reference to a private name, checked when the innermost class ends. */
static bool priv_ref(cunit *p, embi_js_value nm, uint32_t line, uint32_t col)
{
    if (!p->cls) {
        /* Direct eval inside a class body: the names resolve through the caller's scopes. */
        if (p->eval_priv && p->priv_known && p->priv_known(p->priv_user, nm)) {
            return true;
        }
        return perr(p, "Private field must be declared in an enclosing class");
    }
    cpref r = {nm, line, col};
    embi_vec_push(&p->cls->refs, r);
    return true;
}

static bool class_declares(const cclass *c, embi_js_value nm)
{
    for (uint32_t i = 0; i < c->privs.len; i++) {
        if (c->privs.data[i].name == nm) {
            return true;
        }
    }
    return false;
}

/* A child function of parent (its closure is created in parent), whose scope is cs. */
static cfunc *class_child(cunit *p, cfunc *parent, cscope *cs)
{
    cfunc *saved = p->fn;
    p->fn = parent;
    cfunc *f = embi_jsc_new_func(p, parent, cs);
    f->const_idx = embi_jsc_add_const(p, EMBI_JS_EMPTY);
    p->fn = saved;
    CF_PUSH(parent, children, f);
    return f;
}

/* The instance (static) initializer of the class, created with its JMP to the body. */
static cfunc *class_init(cunit *p, cclass *c, bool stat)
{
    cfunc **slot = stat ? &c->sinit : &c->init;
    if (!*slot) {
        cfunc *f = class_child(p, c->owner, c->scope);
        f->is_method = true;
        f->is_field_init = true;
        cfunc *saved = p->fn;
        p->fn = f;
        OPC32(JMP, 0);
        p->fn = saved;
        *slot = f;
    }
    return *slot;
}

/* Temporaries and private names are named bindings of the class scope. */
static void cls_get(cunit *p, const cclass *c, const cbind *b)
{
    embi_jsc_emit_sym(p, SYM_KIND_GET, b->name, c->scope, nullptr);
}

static void cls_put(cunit *p, const cclass *c, const cbind *b)
{
    embi_jsc_emit_sym(p, SYM_KIND_INIT, b->name, c->scope, nullptr);
}

static cbind *cls_temp(cunit *p, cclass *c, char kind)
{
    return embi_jsc_new_bind(p, c->owner, c->scope, tmp_name(p, kind), BK_VAR);
}

/* Declares a private name, or the second accessor of a getter and setter pair. Returns the
 * entry's index. */
static bool declare_private(cunit *p, cclass *c, embi_js_value name, uint8_t kind, bool stat,
                            uint8_t acc, uint32_t *idx)
{
    for (uint32_t i = 0; i < c->privs.len; i++) {
        cpriv *e = &c->privs.data[i];
        if (e->name != name) {
            continue;
        }
        bool pair = e->kind == CP_ACCESSOR && kind == CP_ACCESSOR && e->stat == stat &&
                    ((acc == 1 && !e->has_get) || (acc == 2 && !e->has_set));
        if (!pair) {
            return perr(p, "Identifier has already been declared");
        }
        if (acc == 1) {
            e->has_get = true;
        } else {
            e->has_set = true;
        }
        *idx = i;
        return true;
    }
    cpriv e;
    memset(&e, 0, sizeof e);
    e.name = name;
    e.kind = kind;
    e.stat = stat;
    e.has_get = acc == 1;
    e.has_set = acc == 2;
    e.bind = embi_jsc_new_bind(p, c->owner, c->scope, name, BK_CONST);
    e.bind->init_pc = 0xFFFFFFFFu;
    if (kind == CP_METHOD) {
        e.tm = cls_temp(p, c, 'm');
    } else if (kind == CP_ACCESSOR) {
        e.tg = cls_temp(p, c, 'g');
        e.ts = cls_temp(p, c, 's');
        /* Both sides start undefined: a getter-only or setter-only pair installs undefined. */
        OPC(PUSH_UNDEF);
        cls_put(p, c, e.tg);
        OPC(PUSH_UNDEF);
        cls_put(p, c, e.ts);
    }
    embi_vec_push(&c->privs, e);
    *idx = c->privs.len - 1u;
    return true;
}

/* Parses the rest of a function whose cfunc f was created in advance (a class constructor, or a
 * static block, which has no parameter list when params is false). */
static bool parse_function_rest(cunit *p, cfunc *f);

static bool parse_function_into(cunit *p, cfunc *f, uint32_t src_start, bool params)
{
    cfunc *saved = p->fn;
    ctl *saved_ctl = p->ctl;
    uint32_t saved_block = p->block_depth;
    cblk *saved_blk = p->blk;
    p->fn = f;
    p->ctl = nullptr;
    p->block_depth = 0;
    p->blk = nullptr;
    OPC32(JMP, 0);
    bool ok = params ? parse_function_rest(p, f) : parse_body(p, f);
    p->fn = saved;
    p->ctl = saved_ctl;
    p->block_depth = saved_block;
    p->blk = saved_blk;
    if (!ok) {
        return false;
    }
    f->src_start = src_start;
    f->src_end = p->last_end;
    return true;
}

/* A static block: a method run once with this = the constructor, called from the static
 * initializer in source order. */
static bool parse_static_block(cunit *p, cclass *c)
{
    cfunc *sinit = class_init(p, c, true);
    cfunc *sb = class_child(p, sinit, c->scope);
    sb->is_method = true;
    sb->is_static_block = true;
    cfunc *saved = p->fn;
    p->fn = sinit;
    OPC(PUSH_THIS);
    sb->new_pc = embi_jsc_here(p);
    OPC32(NEW_CLOSURE, sb->const_idx);
    OPC32(SET_HOME, 0);
    OPC(SWAP);
    embi_jsc_emit_op_eff(p, EMBI_JSOP_CALL, 0, 2, 1);
    OPC(POP);
    p->fn = saved;
    return parse_function_into(p, sb, p->tok.start, false);
}

/* The end of a field definition: a semicolon, or a line break or the end of the class body. */
static bool field_end(cunit *p)
{
    if (is(p, EMBI_JST_SEMI)) {
        adv(p);
        return true;
    }
    if (is(p, EMBI_JST_RBRACE) || (p->tok.flags & EMBI_JST_F_NL_BEFORE)) {
        return true;
    }
    return unexpected(p);
}

/* One class element. Methods and accessors are defined in the enclosing function at their place
 * in the class; field initializers are parsed into the initializer functions. */
static bool class_member(cunit *p, cclass *c)
{
    uint32_t item_start = p->tok.start;
    bool stat = false;
    if (is_kw(p, EMBI_JSKW_static)) {
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind != EMBI_JST_LPAREN && nt.kind != EMBI_JST_ASSIGN && nt.kind != EMBI_JST_SEMI &&
            nt.kind != EMBI_JST_RBRACE) {
            stat = true;
            adv(p);
            if (is(p, EMBI_JST_LBRACE)) {
                return parse_static_block(p, c);
            }
            item_start = p->tok.start; /* the source text of a static method omits "static" */
        }
    }
    uint8_t acc = 0; /* 0 none, 1 getter, 2 setter */
    bool async = false;
    if (is_kw(p, EMBI_JSKW_async) && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
        embi_jstok nt;
        peek(p, &nt);
        if (!(nt.flags & EMBI_JST_F_NL_BEFORE) &&
            (nt.kind == EMBI_JST_NAME || nt.kind == EMBI_JST_STRING ||
             nt.kind == EMBI_JST_NUMBER || nt.kind == EMBI_JST_BIGINT ||
             nt.kind == EMBI_JST_LBRACK || nt.kind == EMBI_JST_MUL ||
             nt.kind == EMBI_JST_PRIVATE_NAME)) {
            async = true;
            adv(p);
        }
    }
    bool gen = false;
    if (is(p, EMBI_JST_MUL)) {
        gen = true;
        adv(p);
    } else if (is_kw(p, EMBI_JSKW_get) || is_kw(p, EMBI_JSKW_set)) {
        embi_jstok nt;
        peek(p, &nt);
        /* get, a line break and * : a field named get, then a generator method (ASI). */
        bool asi_gen = nt.kind == EMBI_JST_MUL && (nt.flags & EMBI_JST_F_NL_BEFORE);
        if (nt.kind != EMBI_JST_LPAREN && nt.kind != EMBI_JST_ASSIGN && nt.kind != EMBI_JST_SEMI &&
            nt.kind != EMBI_JST_RBRACE && !asi_gen) {
            acc = is_kw(p, EMBI_JSKW_get) ? 1 : 2;
            adv(p);
        }
    }
    if (async && acc != 0) {
        return perr(p, "Unexpected token in an async method");
    }
    bool priv = false;
    bool computed = false;
    embi_js_value key = EMBI_JS_EMPTY;
    uint32_t kc = 0;
    embi_js_value acc_name = EMBI_JS_EMPTY; /* a private accessor's function name ("get #x") */
    cbind *tk = nullptr;
    if (is(p, EMBI_JST_PRIVATE_NAME)) {
        priv = true;
        key = priv_atom(p);
        if (acc != 0) {
            acc_name = priv_atom_pre(p, acc == 1 ? "get " : "set ");
        }
        if (key == catom(p, "#constructor")) {
            return perr(p, "Classes may not have a private field named '#constructor'");
        }
        adv(p);
    } else if (is(p, EMBI_JST_LBRACK)) {
        computed = true;
        adv(p);
        expr k;
        if (!parse_assign(p, false, &k)) {
            return false;
        }
        to_value(p, &k);
        if (!expect(p, EMBI_JST_RBRACK)) {
            return false;
        }
        OPC(TO_PROPKEY);
        tk = cls_temp(p, c, 'k');
        cls_put(p, c, tk);
    } else {
        if (!property_key(p, &key)) {
            return false;
        }
        adv(p);
        kc = const_str(p, key);
    }
    /* The async prefix is only a method's: `async x` and `async x = 1` are errors. */
    if (async && !is(p, EMBI_JST_LPAREN)) {
        return unexpected(p);
    }
    bool is_ctor = !computed && !priv && key == catom(p, "constructor");
    bool is_proto = !computed && !priv && key == catom(p, "prototype");
    if (is(p, EMBI_JST_LPAREN)) {
        if (is_ctor && !stat) {
            if (acc != 0 || gen || async) {
                return perr(p, "Class constructor may not be an accessor, a generator or async");
            }
            if (c->has_ctor) {
                return perr(p, "A class may only have one constructor");
            }
            c->has_ctor = true;
            return parse_function_into(p, c->ctor, item_start, true);
        }
        if (is_proto && stat) {
            return perr(p, "Classes may not have a static property named 'prototype'");
        }
        uint8_t kind = acc == 1 ? 1 : acc == 2 ? 2 : 0; /* DEFINE_METHOD kind */
        cfunc *m = nullptr;
        if (priv) {
            uint32_t idx = 0;
            embi_js_value fname = key;
            if (acc != 0) {
                fname = acc_name;
            }
            if (!declare_private(p, c, key, acc != 0 ? CP_ACCESSOR : CP_METHOD, stat, acc, &idx)) {
                return false;
            }
            cpriv *e = &c->privs.data[idx];
            cbind *slot = acc == 1 ? e->tg : acc == 2 ? e->ts : e->tm;
            p->next_kind = acc == 1 ? 2 : acc == 2 ? 3 : 1;
            p->next_gen = gen;
            p->next_async = async;
            if (!parse_function_common(p, fname, EMBI_JS_EMPTY, item_start, &m)) {
                return false;
            }
            if (acc != 0 && (acc == 1 ? m->nparams != 0 : m->nparams != 1)) {
                return perr(p, acc == 1 ? "Getter must not have any formal parameters"
                                        : "Setter must have exactly one formal parameter");
            }
            cls_get(p, c, stat ? c->tF : c->tP);
            m->new_pc = embi_jsc_here(p);
            OPC32(NEW_CLOSURE, m->const_idx);
            OPC32(SET_HOME, 0);
            cls_put(p, c, slot);
            OPC(POP);
            return true;
        }
        p->next_kind = acc == 1 ? 2 : acc == 2 ? 3 : 1;
        p->next_gen = gen;
        p->next_async = async;
        if (!parse_function_common(p, EMBI_JS_EMPTY, EMBI_JS_EMPTY, item_start, &m)) {
            return false;
        }
        if (acc != 0 && (acc == 1 ? m->nparams != 0 : m->nparams != 1)) {
            return perr(p, acc == 1 ? "Getter must not have any formal parameters"
                                    : "Setter must have exactly one formal parameter");
        }
        cls_get(p, c, stat ? c->tF : c->tP);
        if (computed) {
            cls_get(p, c, tk);
        } else {
            OPC32(PUSH_CONST, kc);
        }
        m->new_pc = embi_jsc_here(p);
        OPC32(NEW_CLOSURE, m->const_idx);
        embi_jsc_emit_op32(p, EMBI_JSOP_DEFINE_METHOD, kind);
        return true;
    }
    /* A field. */
    if (acc != 0 || gen) {
        return unexpected(p);
    }
    if (is_ctor) {
        return perr(p, "Classes may not have a field named 'constructor'");
    }
    if (is_proto && stat) {
        return perr(p, "Classes may not have a static property named 'prototype'");
    }
    uint32_t idx = 0;
    if (priv && !declare_private(p, c, key, CP_FIELD, stat, 0, &idx)) {
        return false;
    }
    cfunc *f = class_init(p, c, stat);
    cfunc *saved = p->fn;
    p->fn = f;
    if (!computed && !priv) {
        kc = const_str(p, key); /* the initializer's own constant table */
    }
    OPC(PUSH_THIS);
    if (computed) {
        cls_get(p, c, tk);
    } else if (priv) {
        embi_jsc_emit_sym(p, SYM_KIND_GET, key, c->scope, nullptr);
    }
    if (is(p, EMBI_JST_ASSIGN)) {
        adv(p);
        p->hint = computed ? EMBI_JS_EMPTY : key;
        expr v;
        if (!parse_assign(p, false, &v)) {
            return false;
        }
        to_value(p, &v);
        if (computed && v.anon) {
            OPC32(SET_FN_NAME, 0);
        }
    } else {
        OPC(PUSH_UNDEF);
    }
    if (priv) {
        OPC(PRIV_DEF);
    } else if (computed) {
        OPC(DEFINE_COMPUTED);
    } else {
        OPC32(DEFINE_FIELD, kc);
    }
    OPC(POP);
    p->fn = saved;
    return field_end(p);
}

/* Emits %pinit: the private methods and accessors of the instances, created at the end of the
 * class once every private element is known. */
static cfunc *emit_priv_installer(cunit *p, cclass *c)
{
    cfunc *pf = class_child(p, c->owner, c->scope);
    pf->is_method = true;
    cfunc *saved = p->fn;
    p->fn = pf;
    OPC32(JMP, 0);
    for (uint32_t i = 0; i < c->privs.len; i++) {
        const cpriv *e = &c->privs.data[i];
        if (e->stat || e->kind == CP_FIELD) {
            continue;
        }
        OPC(PUSH_THIS);
        embi_jsc_emit_sym(p, SYM_KIND_GET, e->name, c->scope, nullptr);
        if (e->kind == CP_METHOD) {
            cls_get(p, c, e->tm);
            OPC(PRIV_DEF_METHOD);
        } else {
            cls_get(p, c, e->tg);
            cls_get(p, c, e->ts);
            OPC(PRIV_DEF_ACCESSOR);
        }
        OPC(POP);
    }
    OPC(PUSH_UNDEF);
    OPC(RETURN);
    p->fn = saved;
    return pf;
}

/* Emits the body of a class without an explicit constructor. A base class does nothing (the
 * interpreter runs the field initializers at construction); a derived one forwards its arguments
 * to the super constructor. */
static void emit_default_ctor(cunit *p, cclass *c)
{
    cfunc *fc = c->ctor;
    cfunc *saved = p->fn;
    p->fn = fc;
    OPC32(JMP, 0);
    if (!c->derived) {
        emit_implicit_return(p);
    } else {
        embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_fn, fc->cur, nullptr);
        embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_nt, fc->cur, nullptr);
        (void)embi_jsc_ensure_arguments(p, fc);
        embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_arguments, fc->cur, nullptr);
        embi_jsc_emit_op_eff(p, EMBI_JSOP_SUPER_CALL_SPREAD, 0, 3, 1);
        super_bind(p);
        OPC(POP);
        emit_implicit_return(p);
    }
    p->fn = saved;
}

/* The body of a class definition (see parse_class). */
static bool class_parse(cunit *p, expr *e, bool decl, cclass *c)
{
    embi_js_value hint = p->hint;
    p->hint = EMBI_JS_EMPTY;
    uint32_t src_start = p->tok.start;
    adv(p); /* class */
    cfunc *enc = p->fn;
    bool saved_strict = enc->strict;
    enc->strict = true; /* every part of a class is strict code */
    embi_js_value name = EMBI_JS_EMPTY;
    if (is_ident(p) && !is_kw(p, EMBI_JSKW_extends)) {
        name = name_atom(p);
        if (!check_strict_name(p, name)) {
            enc->strict = saved_strict;
            return false;
        }
        adv(p);
    } else if (decl) {
        enc->strict = saved_strict;
        return unexpected(p);
    }
    cbind *outer_b = nullptr;
    if (decl) {
        bool ok;
        outer_b = declare_lexical(p, name, BK_LET, &ok);
        if (!ok) {
            enc->strict = saved_strict;
            return false;
        }
    }
    cscope *saved_cur = enc->cur;
    c->outer = p->cls;
    c->owner = enc;
    c->scope = embi_jsc_new_scope(p, saved_cur);
    if (name != EMBI_JS_EMPTY) {
        c->name_b = embi_jsc_new_bind(p, enc, c->scope, name, BK_CONST);
        c->name_b->init_pc = 0xFFFFFFFFu;
    }
    enc->cur = c->scope;
    p->cls = c;
    int32_t depth0 = enc->depth;
    uint32_t tail_jmp = jump_emit(p, EMBI_JSOP_JMP);
    uint32_t resume = embi_jsc_here(p);
    c->tF = cls_temp(p, c, 'F');
    c->tP = cls_temp(p, c, 'P');
    if (is_kw(p, EMBI_JSKW_extends)) {
        adv(p);
        expr h;
        /* The heritage is evaluated in the outer private environment: the class's own private names
         * are not in scope there. */
        p->cls = c->outer;
        bool hok = parse_lhs_ex(p, &h, false);
        p->cls = c;
        if (!hok) {
            return false;
        }
        to_value(p, &h);
        c->derived = true;
    } else {
        OPC(PUSH_EMPTY);
    }
    cfunc *fc = class_child(p, enc, c->scope);
    fc->is_method = true;
    fc->is_class_ctor = true;
    fc->is_derived = c->derived;
    fc->name = name != EMBI_JS_EMPTY ? name : hint;
    c->ctor = fc;
    fc->new_pc = embi_jsc_here(p);
    OPC32(NEW_CLOSURE, fc->const_idx);
    OPC(CLASS_SETUP);
    cls_put(p, c, c->tP);
    cls_put(p, c, c->tF);
    /* The body. */
    if (!expect(p, EMBI_JST_LBRACE)) {
        return false;
    }
    while (!is(p, EMBI_JST_RBRACE)) {
        if (is(p, EMBI_JST_EOF)) {
            return unexpected(p);
        }
        if (is(p, EMBI_JST_SEMI)) {
            adv(p);
            continue;
        }
        if (!class_member(p, c)) {
            return false;
        }
    }
    adv(p); /* } */
    fc->src_start = src_start;
    fc->src_end = p->last_end;
    if (!c->has_ctor) {
        emit_default_ctor(p, c);
    }
    /* Private names referenced here must be declared here or in an enclosing class. */
    for (uint32_t i = 0; i < c->refs.len; i++) {
        cpref r = c->refs.data[i];
        if (class_declares(c, r.name)) {
            continue;
        }
        if (!c->outer) {
            if (p->eval_priv && p->priv_known && p->priv_known(p->priv_user, r.name)) {
                continue;
            }
            return perr(p, "Private field must be declared in an enclosing class");
        }
        embi_vec_push(&c->outer->refs, r);
    }
    /* Instance initializers: the private installer and the field initializer, set on F. */
    bool any_inst_priv = false;
    for (uint32_t i = 0; i < c->privs.len; i++) {
        any_inst_priv = any_inst_priv || (!c->privs.data[i].stat && c->privs.data[i].kind != CP_FIELD);
    }
    cfunc *pinit = any_inst_priv ? emit_priv_installer(p, c) : nullptr;
    if (c->init) {
        p->fn = c->init;
        emit_implicit_return(p);
        p->fn = enc;
    }
    if (c->sinit) {
        p->fn = c->sinit;
        emit_implicit_return(p);
        p->fn = enc;
    }
    if (pinit || c->init) {
        cls_get(p, c, c->tF);
        cls_get(p, c, c->tP);
        if (pinit) {
            pinit->new_pc = embi_jsc_here(p);
            OPC32(NEW_CLOSURE, pinit->const_idx);
        } else {
            OPC(PUSH_UNDEF);
        }
        OPC32(SET_HOME, 0);
        if (c->init) {
            c->init->new_pc = embi_jsc_here(p);
            OPC32(NEW_CLOSURE, c->init->const_idx);
        } else {
            OPC(PUSH_UNDEF);
        }
        OPC32(SET_HOME, 1);
        OPC(CLASS_SET_INIT);
    }
    /* Static private methods and accessors are defined before the static elements run. */
    for (uint32_t i = 0; i < c->privs.len; i++) {
        const cpriv *pe = &c->privs.data[i];
        if (!pe->stat || pe->kind == CP_FIELD) {
            continue;
        }
        cls_get(p, c, c->tF);
        embi_jsc_emit_sym(p, SYM_KIND_GET, pe->name, c->scope, nullptr);
        if (pe->kind == CP_METHOD) {
            cls_get(p, c, pe->tm);
            OPC(PRIV_DEF_METHOD);
        } else {
            cls_get(p, c, pe->tg);
            cls_get(p, c, pe->ts);
            OPC(PRIV_DEF_ACCESSOR);
        }
        OPC(POP);
    }
    /* The inner name is initialized before the static elements run. */
    if (c->name_b) {
        cls_get(p, c, c->tF);
        cls_put(p, c, c->name_b);
    }
    if (c->sinit) {
        cls_get(p, c, c->tF);
        c->sinit->new_pc = embi_jsc_here(p);
        OPC32(NEW_CLOSURE, c->sinit->const_idx);
        OPC(CLASS_RUN_STATIC);
    }
    cls_get(p, c, c->tF);
    /* Leave the scope and the strictness of the enclosing code. */
    enc->cur = saved_cur;
    enc->strict = saved_strict;
    p->cls = c->outer;
    if (decl) {
        outer_b->init_pc = embi_jsc_here(p);
        embi_jsc_emit_sym(p, SYM_KIND_INIT, name, saved_cur, outer_b);
    } else {
        set_value(e);
        e->anon = name == EMBI_JS_EMPTY;
    }
    /* The tail: creates the private names (and the inner name's box) before the class starts, then
     * jumps back to the resume point. */
    uint32_t end_jmp = jump_emit(p, EMBI_JSOP_JMP);
    int32_t depth_end = enc->depth;
    uint32_t tail = embi_jsc_here(p);
    enc->depth = depth0;
    for (uint32_t i = 0; i < c->privs.len; i++) {
        uint32_t k = embi_jsc_add_const(p, c->privs.data[i].name);
        embi_jsc_emit_op32(p, EMBI_JSOP_NEW_PRIVATE, k);
        embi_jsc_emit_sym(p, SYM_KIND_INIT, c->privs.data[i].name, c->scope, nullptr);
    }
    if (c->name_b) {
        OPC(PUSH_EMPTY);
        c->name_b->prebox = true;
        c->name_b->stub_pc = embi_jsc_here(p);
        embi_jsc_emit_sym(p, SYM_KIND_INIT, name, c->scope, nullptr);
    }
    embi_jsc_emit_op32(p, EMBI_JSOP_JMP, resume);
    jump_to(p, tail_jmp, tail);
    jump_to(p, end_jmp, embi_jsc_here(p));
    enc->depth = depth_end;
    return true;
}

/* A class definition: a ClassDeclaration when decl (its binding is initialized), else a
 * ClassExpression whose value is left in e. */
static bool parse_class(cunit *p, expr *e, bool decl)
{
    NEST_GUARD(p);
    cclass c;
    memset(&c, 0, sizeof c);
    bool ok = class_parse(p, e, decl, &c);
    p->cls = c.outer;
    embi_vec_free(&c.privs);
    embi_vec_free(&c.refs);
    return ok;
}

/* A function expression (ES2015 14.1, and ES2017 14.7 for async): the current token is
 * function. start is the source start of the text (the async keyword when async). */
static bool parse_function_expr(cunit *p, expr *e, uint32_t start, bool async, embi_js_value hint)
{
    adv(p);
    bool gen = false;
    if (is(p, EMBI_JST_MUL)) {
        gen = true;
        adv(p);
    }
    embi_js_value self = EMBI_JS_EMPTY;
    embi_js_value nm = hint;
    /* The name of a function expression is parsed with [~Await], so `await` is a legal
     * name even inside a class static block. */
    /* The name of a non-generator function expression is [~Yield], so yield is a legal name in a
     * sloppy generator body (strict code rejects it through is_ident). */
    bool yield_name = !gen && !p->fn->strict && p->tok.kind == EMBI_JST_NAME &&
                      p->tok.kw == EMBI_JSKW_yield;
    if (is_ident(p) || yield_name || (p->tok.kind == EMBI_JST_NAME && p->tok.kw == EMBI_JSKW_await)) {
        /* The name of a generator expression is in the scope of yield, and that of an async
         * function expression in the scope of await (ECMA-262 15.5.1, 15.8.1, 15.6.1). */
        if ((gen && is_kw(p, EMBI_JSKW_yield)) || (async && is_kw(p, EMBI_JSKW_await))) {
            return perr(p, "This name is not a valid function expression name here");
        }
        self = name_atom(p);
        nm = self;
        if (p->dynfn && p->fn->is_script && !p->dynfn_seen) {
            /* Function constructor source: the name is only the text of toString. */
            p->dynfn_seen = true;
            self = EMBI_JS_EMPTY;
        }
        adv(p);
    }
    cfunc *f;
    p->next_gen = gen;
    p->next_async = async;
    if (!parse_function_common(p, nm, self, start, &f)) {
        return false;
    }
    f->new_pc = embi_jsc_here(p);
    OPC32(NEW_CLOSURE, f->const_idx);
    e->kind = EX_VALUE;
    e->anon = self == EMBI_JS_EMPTY;
    return true;
}

/* A RegExp literal at the current token (J4c). The pattern is compiled now, so a bad pattern is an
 * early SyntaxError. The compiled program lives in a template constant, and NEW_REGEXP clones the
 * template on each evaluation. */
/* UTF-8 (or WTF-8: a three-byte lone surrogate) to UTF-16. units holds at least len entries; every
 * byte yields at most one unit. Malformed bytes become U+FFFD. */
static size_t wtf8_to_utf16(const uint8_t *s, size_t len, uint16_t *units)
{
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        uint8_t c = s[i];
        uint32_t cp = 0xFFFDu;
        size_t w = 1;
        if (c < 0x80u) {
            cp = c;
        } else if (c >= 0xC2u && c <= 0xDFu && i + 1u < len && (s[i + 1u] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c & 0x1Fu) << 6) | (s[i + 1u] & 0x3Fu);
            w = 2;
        } else if (c >= 0xE0u && c <= 0xEFu && i + 2u < len && (s[i + 1u] & 0xC0u) == 0x80u &&
                   (s[i + 2u] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c & 0x0Fu) << 12) | ((uint32_t)(s[i + 1u] & 0x3Fu) << 6) |
                 (s[i + 2u] & 0x3Fu);
            w = 3;
            if (cp < 0x800u) {
                cp = 0xFFFDu;
            }
        } else if (c >= 0xF0u && c <= 0xF4u && i + 3u < len && (s[i + 1u] & 0xC0u) == 0x80u &&
                   (s[i + 2u] & 0xC0u) == 0x80u && (s[i + 3u] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c & 0x07u) << 18) | ((uint32_t)(s[i + 1u] & 0x3Fu) << 12) |
                 ((uint32_t)(s[i + 2u] & 0x3Fu) << 6) | (s[i + 3u] & 0x3Fu);
            w = 4;
            if (cp < 0x10000u || cp > 0x10FFFFu) {
                cp = 0xFFFDu;
            }
        }
        i += w;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            units[n++] = (uint16_t)(0xD800u + (cp >> 10));
            units[n++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
        } else {
            units[n++] = (uint16_t)cp;
        }
    }
    return n;
}

static bool parse_regexp_literal(cunit *p, expr *e)
{
    (void)e;
    const uint32_t body_start = p->tok.start + 1u;
    const uint32_t body_len = p->tok.aux_end - body_start;
    const uint32_t flags_start = p->tok.aux_end + 1u;
    const uint32_t flags_len = p->tok.end - flags_start;
    /* The lexer admits at most the eight flag letters, each once; anything longer is malformed. */
    if (flags_len > 8u) {
        return perr(p, "Invalid regular expression flags");
    }
    uint16_t *units = embi_alloc(((size_t)body_len + 1u) * sizeof(uint16_t));
    size_t n = wtf8_to_utf16(p->src + body_start, body_len, units);
    uint16_t fbuf[8];
    for (uint32_t i = 0; i < flags_len; i++) {
        fbuf[i] = p->src[flags_start + i];
    }
    uint32_t flags = 0;
    if (!embi_re_parse_flags(fbuf, flags_len, &flags)) {
        embi_free(units, ((size_t)body_len + 1u) * sizeof(uint16_t));
        return perr(p, "Invalid regular expression flags");
    }
    embi_re_error err = {0, nullptr};
    embi_re_prog *prog = embi_re_compile(units, (uint32_t)n, flags, &err);
    if (!prog) {
        embi_free(units, ((size_t)body_len + 1u) * sizeof(uint16_t));
        return perr(p, "Invalid regular expression: /%.*s/%.*s: %s", (int)body_len,
                    (const char *)p->src + body_start, (int)flags_len,
                    (const char *)p->src + flags_start, err.msg ? err.msg : "invalid pattern");
    }
    embi_js_value tmpl = embi_js_regexp_template_new(p->rt, prog, flags, units, (uint32_t)n);
    embi_free(units, ((size_t)body_len + 1u) * sizeof(uint16_t));
    if (embi_js_is_exception(tmpl)) {
        return false;
    }
    OPC32(NEW_REGEXP, const_str(p, tmpl));
    adv(p);
    return true;
}

static bool parse_primary(cunit *p, expr *e)
{
    embi_js_value hint = p->hint;
    p->hint = EMBI_JS_EMPTY;
    memset(e, 0, sizeof *e);
    e->kind = EX_VALUE;
    e->line = p->tok.line;
    e->col = p->tok.col;
    uint32_t start = p->tok.start;

    switch (p->tok.kind) {
    case EMBI_JST_NAME:
        if (p->tok.kw == EMBI_JSKW_async && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
            embi_jstok nt;
            peek(p, &nt);
            if (nt.kind == EMBI_JST_NAME && !(nt.flags & EMBI_JST_F_NL_BEFORE) &&
                nt.kw == EMBI_JSKW_function && !(nt.flags & EMBI_JST_F_ESCAPED)) {
                adv(p); /* async */
                return parse_function_expr(p, e, start, true, hint);
            }
        }
        if (!(p->tok.flags & EMBI_JST_F_ESCAPED)) {
            if (p->tok.kw == EMBI_JSKW_super) {
                return parse_super(p, e);
            }
            if (p->tok.kw == EMBI_JSKW_class) {
                p->hint = hint;
                return parse_class(p, e, false);
            }
            if (p->tok.kw == EMBI_JSKW_import) {
                return parse_import_expr(p, e);
            }
            if (p->tok.kw == EMBI_JSKW_export) {
                return perr(p, "Unexpected token 'export'");
            }
        }
        if (p->tok.kw == EMBI_JSKW_this && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
            /* An arrow function's this is the enclosing function's, through the %this binding. */
            emit_this(p);
            adv(p);
            return true;
        }
        if (is_kw(p, EMBI_JSKW_null)) {
            OPC(PUSH_NULL);
            adv(p);
            return true;
        }
        if (is_kw(p, EMBI_JSKW_true)) {
            OPC(PUSH_TRUE);
            adv(p);
            return true;
        }
        if (is_kw(p, EMBI_JSKW_false)) {
            OPC(PUSH_FALSE);
            adv(p);
            return true;
        }
        if (is_kw(p, EMBI_JSKW_function)) {
            return parse_function_expr(p, e, start, false, hint);
        }
        if (is_kw(p, EMBI_JSKW_new)) {
            return perr(p, "internal: new in primary");
        }
        if (!is_ident(p)) {
            return unexpected(p);
        }
        if (name_atom(p) == p->atom_arguments) {
            cfunc *g = nonarrow(p);
            if (g && (g->is_field_init || g->is_static_block || (g->is_eval && p->eval_noargs))) {
                return perr(p, "'arguments' is not allowed in class field initializer or static initialization block");
            }
        }
        e->kind = EX_VAR;
        e->name = name_atom(p);
        e->scope = p->fn->cur;
        adv(p);
        return true;
    case EMBI_JST_NUMBER: {
        if ((p->tok.flags & EMBI_JST_F_LEGACY) && p->fn->strict) {
            return perr(p, "Octal literals and decimals with leading zeros are not allowed in strict mode");
        }
        embi_js_value v = embi_js_number(p->tok.num);
        if (embi_js_is_int32(v)) {
            OPC32(PUSH_I32, (uint32_t)embi_js_as_int32(v));
        } else {
            OPC32(PUSH_CONST, const_str(p, v));
        }
        adv(p);
        return true;
    }
    case EMBI_JST_STRING: {
        if ((p->tok.flags & EMBI_JST_F_LEGACY) && p->fn->strict) {
            return perr(p, "Octal escape sequences are not allowed in strict mode");
        }
        embi_js_value v = embi_js_from_string(
            embi_js_str_new_utf16(p->rt, p->tok.cooked, p->tok.cooked_len));
        OPC32(PUSH_CONST, const_str(p, v));
        adv(p);
        return true;
    }
    case EMBI_JST_LPAREN: {
        /* An arrow function's parameter list was recognised before this point (parse_assign). */
        adv(p);
        /* An optional chain at the start keeps its reference, so that (a?.b)() has a this value. */
        p->keep_ref_once = true;
        p->hint = hint; /* (function () {}) is still an anonymous function definition */
        if (!parse_expression(p, false, e)) {
            return false;
        }
        p->keep_ref_once = false;
        p->hint = EMBI_JS_EMPTY;
        if (!expect(p, EMBI_JST_RPAREN)) {
            return false;
        }
        e->paren = true;
        return true;
    }
    case EMBI_JST_PRIVATE_NAME:
        return unexpected(p);
    case EMBI_JST_LBRACK:
        return parse_array_literal(p, e);
    case EMBI_JST_LBRACE:
        return parse_object_literal(p, e);
    case EMBI_JST_DIV:
    case EMBI_JST_DIV_EQ:
        /* In operand position a slash starts a regular expression literal (J4c). */
        embi_jslex_rescan_regex(&p->lx, &p->tok);
        if (p->tok.kind != EMBI_JST_REGEX) {
            return unexpected(p);
        }
        return parse_regexp_literal(p, e);
    case EMBI_JST_REGEX:
        return parse_regexp_literal(p, e);
    case EMBI_JST_TPL_NOSUB:
    case EMBI_JST_TPL_HEAD:
        return parse_template(p, e);
    case EMBI_JST_BIGINT: {
        embi_js_value v = bigint_token_value(p);
        if (embi_js_is_exception(v)) {
            return false;
        }
        OPC32(PUSH_CONST, const_str(p, v));
        adv(p);
        return true;
    }
    default:
        return unexpected(p);
    }
}


static bool parse_new(cunit *p, expr *e)
{
    NEST_GUARD(p);
    adv(p); /* new */
    if (is(p, EMBI_JST_DOT)) {
        adv(p);
        if (!tok_is_word(p, "target")) {
            return unexpected(p);
        }
        adv(p);
        if (!new_target_allowed(p)) {
            return perr(p, "new.target expression is not allowed here");
        }
        if (p->fn->is_arrow) {
            embi_jsc_emit_sym(p, SYM_KIND_GET, p->atom_nt, p->fn->cur, nullptr);
        } else {
            OPC(PUSH_NEW_TARGET);
        }
        set_value(e);
        return true;
    }
    expr callee;
    if (is_kw(p, EMBI_JSKW_import)) {
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind == EMBI_JST_LPAREN) {
            return perr(p, "Cannot use new with import()");
        }
    }
    if (is_kw(p, EMBI_JSKW_new)) {
        if (!parse_new(p, &callee)) {
            return false;
        }
    } else {
        if (!parse_primary(p, &callee)) {
            return false;
        }
    }
    if (!parse_call_member(p, &callee, false)) {
        return false;
    }
    to_value(p, &callee);
    uint32_t argc = 0;
    bool spread = false;
    if (is(p, EMBI_JST_LPAREN)) {
        uint32_t line = p->tok.line;
        uint32_t col = p->tok.col;
        if (!parse_arguments(p, &argc, &spread)) {
            return false;
        }
        mark(p, line, col);
    }
    if (spread) {
        embi_jsc_emit_op_eff(p, EMBI_JSOP_NEW_SPREAD, 0, 2, 1);
    } else {
        embi_jsc_emit_op_eff(p, EMBI_JSOP_NEW, argc, (int)argc + 1, 1);
    }
    set_value(e);
    return true;
}


static bool parse_lhs_ex(cunit *p, expr *e, bool keep_ref)
{
    uint32_t start = p->tok.start;
    if (is_kw(p, EMBI_JSKW_new)) {
        if (!parse_new(p, e)) {
            return false;
        }
    } else if (!parse_primary(p, e)) {
        return false;
    }
    e->start = start;
    return parse_call_member_ex(p, e, true, keep_ref);
}

/* Member, index, call and tagged-template links, with optional chains. An optional link (?.)
 * tests its base: when that is nullish, OPT_NULLISH pops the chain's operands and pushes
 * undefined, and jumps past the end of the chain. Every such jump of one chain is patched to the
 * end. A chain whose last link is a reference stays unloaded when keep_ref is set (the operand of
 * delete): its jumps are then carried in e->opt. */


static bool parse_call_member_ex(cunit *p, expr *e, bool allow_call, bool keep_ref)
{
    cpatch *opt = nullptr; /* OPT_NULLISH sites that jump to the end of the chain */
    bool opt_next = false; /* the next call link is optional: f?.() */
    for (;;) {
        bool optional = is(p, EMBI_JST_OPTCHAIN);
        if (optional) {
            if (!allow_call) {
                return perr(p, "Invalid optional chain from new expression");
            }
            adv(p);
            if (is(p, EMBI_JST_LPAREN)) {
                opt_next = true;
            } else {
                to_value(p, e);
                uint32_t pc = embi_jsc_emit_op2(p, EMBI_JSOP_OPT_NULLISH, 0, 0);
                opt = embi_jsc_patch_add(p, opt, pc + 1u);
            }
        }
        if (is(p, EMBI_JST_DOT) || (optional && !is(p, EMBI_JST_LBRACK) && !is(p, EMBI_JST_LPAREN))) {
            uint32_t line = p->tok.line;
            uint32_t col = p->tok.col;
            to_value(p, e);
            if (is(p, EMBI_JST_DOT)) {
                adv(p);
            }
            if (p->tok.kind == EMBI_JST_PRIVATE_NAME) {
                embi_js_value nm = priv_atom(p);
                if (!priv_ref(p, nm, line, col)) {
                    return false;
                }
                embi_jsc_emit_sym(p, SYM_KIND_GET, nm, p->fn->cur, nullptr); /* the key */
                adv(p);
                e->kind = EX_PRIV;
                e->name = nm;
                e->line = line;
                e->col = col;
            } else {
                if (p->tok.kind != EMBI_JST_NAME) {
                    return unexpected(p);
                }
                embi_js_value key = name_atom(p);
                adv(p);
                e->kind = EX_FIELD;
                e->name = key;
                e->key = const_str(p, key);
                e->line = line;
                e->col = col;
            }
        } else if (is(p, EMBI_JST_LBRACK)) {
            uint32_t line = p->tok.line;
            uint32_t col = p->tok.col;
            to_value(p, e);
            adv(p);
            expr idx;
            if (!parse_expression(p, false, &idx)) {
                return false;
            }
            to_value(p, &idx);
            if (!expect(p, EMBI_JST_RBRACK)) {
                return false;
            }
            e->kind = EX_INDEX;
            e->line = line;
            e->col = col;
        } else if (is(p, EMBI_JST_LPAREN) && allow_call) {
            uint32_t line = p->tok.line;
            uint32_t col = p->tok.col;
            uint32_t name_k = callee_text_const(p, e->start, p->last_end);
            bool direct_eval = !opt_next && e->kind == EX_VAR && !e->paren && e->name == p->atom_eval;
            emit_callee(p, e);
            if (opt_next) {
                /* the callee is below its this value: the test reads one value further down */
                uint32_t pc = embi_jsc_emit_op2(p, EMBI_JSOP_OPT_NULLISH, 0, 1);
                opt = embi_jsc_patch_add(p, opt, pc + 1u);
                opt_next = false;
            }
            uint32_t argc = 0;
            bool spread = false;
            if (!parse_arguments(p, &argc, &spread)) {
                return false;
            }
            mark(p, line, col);
            if (spread && direct_eval) {
                uint32_t site = new_eval_site(p, 0, name_k);
                embi_jsc_emit_op_eff(p, EMBI_JSOP_CALL_EVAL_SPREAD, site, 3, 1);
            } else if (spread) {
                embi_jsc_emit_op_eff(p, EMBI_JSOP_CALL_SPREAD, 0, 3, 1);
            } else if (direct_eval) {
                uint32_t site = new_eval_site(p, argc, name_k);
                embi_jsc_emit_op_eff(p, EMBI_JSOP_CALL_EVAL, site, (int)argc + 2, 1);
            } else {
                embi_jsc_emit_op_eff(p, EMBI_JSOP_CALL, argc | (name_k << 16), (int)argc + 2, 1);
            }
            set_value(e);
        } else if (is(p, EMBI_JST_TPL_NOSUB) || is(p, EMBI_JST_TPL_HEAD)) {
            if (opt || opt_next) {
                return perr(p, "Invalid tagged template on optional chain");
            }
            emit_callee(p, e);
            uint32_t argc = 0;
            if (!parse_tagged_template(p, &argc)) {
                return false;
            }
            embi_jsc_emit_op_eff(p, EMBI_JSOP_CALL, argc, (int)argc + 2, 1);
            set_value(e);
        } else {
            break;
        }
    }
    if (opt) {
        if (keep_ref && (e->kind == EX_FIELD || e->kind == EX_INDEX)) {
            e->opt = opt;
        } else {
            to_value(p, e);
            embi_jsc_patch_list(p, opt, embi_jsc_here(p));
        }
    }
    return true;
}


static bool parse_call_member(cunit *p, expr *e, bool allow_call)
{
    return parse_call_member_ex(p, e, allow_call, false);
}


static bool parse_postfix_ex(cunit *p, expr *e, bool keep_ref)
{
    if (!parse_lhs_ex(p, e, keep_ref)) {
        return false;
    }
    if ((is(p, EMBI_JST_INC) || is(p, EMBI_JST_DEC)) && !(p->tok.flags & EMBI_JST_F_NL_BEFORE)) {
        if (!assignable(e) || e->opt) {
            return perr(p, "Invalid left-hand side expression in postfix operation");
        }
        if (!check_target(p, e)) {
            return false;
        }
        bool inc = is(p, EMBI_JST_INC);
        adv(p);
        emit_postfix(p, e, inc);
        set_value(e);
    }
    return true;
}

/* keep_ref_once is set by delete just before its operand: the operand's outermost chain then
 * keeps its reference (see parse_call_member_ex). It is consumed on entry, so nested operands
 * never see it. */

static bool parse_unary(cunit *p, expr *e)
{
    NEST_GUARD(p);
    bool keep = p->keep_ref_once;
    p->keep_ref_once = false;
    if (p->fn->is_async && is_kw(p, EMBI_JSKW_await)) {
        if (p->fn->in_params) {
            return perr(p, "await expressions are not allowed in async function parameters");
        }
        adv(p);
        if (p->fn == p->top) {
            p->fn->uses_await = true;
        }
        expr x;
        if (!parse_unary(p, &x)) {
            return false;
        }
        to_value(p, &x);
        OPC(AWAIT);
        OPC32(GEN_RESUME, 0);
        set_value(e);
        return true;
    }
    if (is_kw(p, EMBI_JSKW_delete)) {
        adv(p);
        expr x;
        p->keep_ref_once = true;
        if (!parse_unary(p, &x)) {
            return false;
        }
        if (x.kind == EX_VAR) {
            if (p->fn->strict) {
                return perr(p, "Delete of an unqualified identifier in strict mode.");
            }
            embi_jsc_emit_sym(p, SYM_KIND_DELETE, x.name, x.scope, nullptr);
        } else if (x.kind == EX_FIELD) {
            OPC32(DELETE_FIELD, x.key);
        } else if (x.kind == EX_INDEX) {
            OPC(DELETE_INDEX);
        } else if (x.kind == EX_SUPER_FIELD) {
            OPC(SUPER_DELETE);
        } else if (x.kind == EX_SUPER_INDEX) {
            OPC(POP); /* the key was evaluated; the base is [this home] */
            OPC(SUPER_DELETE);
        } else if (x.kind == EX_PRIV) {
            return perr(p, "Private fields can not be deleted");
        } else {
            to_value(p, &x);
            OPC(POP);
            OPC(PUSH_TRUE);
        }
        if ((x.kind == EX_FIELD || x.kind == EX_INDEX) && x.opt) {
            /* a short-circuited optional chain deletes nothing and yields true */
            uint32_t j = jump_emit(p, EMBI_JSOP_JMP);
            embi_jsc_patch_list(p, x.opt, embi_jsc_here(p));
            OPC(POP);
            OPC(PUSH_TRUE);
            jump_to(p, j, embi_jsc_here(p));
        }
        set_value(e);
        return true;
    }
    if (is_kw(p, EMBI_JSKW_void)) {
        adv(p);
        expr x;
        if (!parse_unary(p, &x)) {
            return false;
        }
        to_value(p, &x);
        OPC(POP);
        OPC(PUSH_UNDEF);
        set_value(e);
        return true;
    }
    if (is_kw(p, EMBI_JSKW_typeof)) {
        adv(p);
        expr x;
        if (!parse_unary(p, &x)) {
            return false;
        }
        if (x.kind == EX_VAR) {
            embi_jsc_emit_sym(p, SYM_KIND_TYPEOF, x.name, x.scope, nullptr);
        } else {
            to_value(p, &x);
        }
        OPC(TYPEOF);
        set_value(e);
        return true;
    }
    uint8_t unop = 0;
    switch (p->tok.kind) {
    case EMBI_JST_ADD:
        unop = EMBI_JSOP_PLUS;
        break;
    case EMBI_JST_SUB:
        unop = EMBI_JSOP_NEG;
        break;
    case EMBI_JST_BNOT:
        unop = EMBI_JSOP_BNOT;
        break;
    case EMBI_JST_NOT:
        unop = EMBI_JSOP_NOT;
        break;
    default:
        break;
    }
    if (unop) {
        adv(p);
        expr x;
        if (!parse_unary(p, &x)) {
            return false;
        }
        to_value(p, &x);
        embi_jsc_emit_op(p, unop);
        set_value(e);
        return true;
    }
    if (is(p, EMBI_JST_INC) || is(p, EMBI_JST_DEC)) {
        bool inc = is(p, EMBI_JST_INC);
        adv(p);
        expr x;
        if (!parse_unary(p, &x)) {
            return false;
        }
        if (!assignable(&x) || x.opt) {
            return perr(p, "Invalid left-hand side expression in prefix operation");
        }
        if (!check_target(p, &x)) {
            return false;
        }
        emit_prefix(p, &x, inc);
        set_value(e);
        return true;
    }
    return parse_postfix_ex(p, e, keep);
}

/* Binary operators by precedence (see binop_info). ?? has the precedence of || and may not be
 * mixed with || or && without parentheses. Its right operand binds tighter than the logical
 * operators, so a mixed chain reaches the mixing check. */

/* Binary operator precedence (0 when the token is not a binary operator here). */
static int binop_info(const cunit *p, bool no_in, uint8_t *op)
{
    switch (p->tok.kind) {
    case EMBI_JST_LOR:
        *op = EMBI_JSOP_NOP;
        return 1;
    case EMBI_JST_LAND:
        *op = EMBI_JSOP_NOP;
        return 2;
    case EMBI_JST_BOR:
        *op = EMBI_JSOP_BOR;
        return 3;
    case EMBI_JST_BXOR:
        *op = EMBI_JSOP_BXOR;
        return 4;
    case EMBI_JST_BAND:
        *op = EMBI_JSOP_BAND;
        return 5;
    case EMBI_JST_EQ:
        *op = EMBI_JSOP_EQ;
        return 6;
    case EMBI_JST_NE:
        *op = EMBI_JSOP_NE;
        return 6;
    case EMBI_JST_SEQ:
        *op = EMBI_JSOP_SEQ;
        return 6;
    case EMBI_JST_SNE:
        *op = EMBI_JSOP_SNE;
        return 6;
    case EMBI_JST_LT:
        *op = EMBI_JSOP_LT;
        return 7;
    case EMBI_JST_GT:
        *op = EMBI_JSOP_GT;
        return 7;
    case EMBI_JST_LE:
        *op = EMBI_JSOP_LE;
        return 7;
    case EMBI_JST_GE:
        *op = EMBI_JSOP_GE;
        return 7;
    case EMBI_JST_SHL:
        *op = EMBI_JSOP_SHL;
        return 8;
    case EMBI_JST_SAR:
        *op = EMBI_JSOP_SAR;
        return 8;
    case EMBI_JST_SHR:
        *op = EMBI_JSOP_SHR;
        return 8;
    case EMBI_JST_ADD:
        *op = EMBI_JSOP_ADD;
        return 9;
    case EMBI_JST_SUB:
        *op = EMBI_JSOP_SUB;
        return 9;
    case EMBI_JST_MUL:
        *op = EMBI_JSOP_MUL;
        return 10;
    case EMBI_JST_DIV:
        *op = EMBI_JSOP_DIV;
        return 10;
    case EMBI_JST_MOD:
        *op = EMBI_JSOP_MOD;
        return 10;
    case EMBI_JST_EXP:
        *op = EMBI_JSOP_EXP;
        return 11;
    case EMBI_JST_NULLISH:
        *op = EMBI_JSOP_NOP;
        return 1;
    case EMBI_JST_NAME:
        if (p->tok.flags & EMBI_JST_F_ESCAPED) {
            return 0;
        }
        if (p->tok.kw == EMBI_JSKW_instanceof) {
            *op = EMBI_JSOP_INSTANCEOF;
            return 7;
        }
        if (p->tok.kw == EMBI_JSKW_in && !no_in) {
            *op = EMBI_JSOP_IN;
            return 7;
        }
        return 0;
    default:
        return 0;
    }
}

static bool parse_binary(cunit *p, int min_prec, bool no_in, expr *e)
{
    expr lhs;
    bool unary_lhs = is(p, EMBI_JST_ADD) || is(p, EMBI_JST_SUB) || is(p, EMBI_JST_BNOT) ||
                     is(p, EMBI_JST_NOT) || is_kw(p, EMBI_JSKW_delete) ||
                     is_kw(p, EMBI_JSKW_void) || is_kw(p, EMBI_JSKW_typeof);
    bool priv_in = false; /* the left operand is a private name: #x in obj */
    if (is(p, EMBI_JST_PRIVATE_NAME)) {
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind != EMBI_JST_NAME || nt.kw != EMBI_JSKW_in || no_in || min_prec > 7) {
            return unexpected(p);
        }
        embi_js_value nm = priv_atom(p);
        if (!priv_ref(p, nm, p->tok.line, p->tok.col)) {
            return false;
        }
        embi_jsc_emit_sym(p, SYM_KIND_GET, nm, p->fn->cur, nullptr);
        adv(p);
        set_value(&lhs);
        priv_in = true;
    } else if (!parse_unary(p, &lhs)) {
        return false;
    }
    bool logical_seen = false; /* && or || at this level */
    bool nullish_seen = false; /* ?? at this level */
    for (;;) {
        uint8_t op = 0;
        int prec = binop_info(p, no_in, &op);
        if (prec == 0 || prec < min_prec) {
            break;
        }
        bool this_priv_in = priv_in;
        priv_in = false;
        if (op == EMBI_JSOP_EXP && unary_lhs) {
            return perr(p, "Unary operator used immediately before exponentiation expression");
        }
        unary_lhs = false;
        uint8_t kind = p->tok.kind;
        bool nullish = kind == EMBI_JST_NULLISH;
        bool logical = kind == EMBI_JST_LAND || kind == EMBI_JST_LOR;
        if (nullish && logical_seen) {
            return perr(p, "Unexpected token '?\?'");
        }
        if (logical && nullish_seen) {
            return perr(p, "Unexpected token");
        }
        adv(p);
        to_value(p, &lhs);
        if (nullish) {
            uint32_t j = jump_emit(p, EMBI_JSOP_JMP_NOT_NULLISH_KEEP);
            p->fn->depth -= 1; /* the fall-through pops the operand */
            expr rhs;
            if (!parse_binary(p, 3, no_in, &rhs)) {
                return false;
            }
            to_value(p, &rhs);
            jump_to(p, j, embi_jsc_here(p));
            nullish_seen = true;
        } else if (logical) {
            uint32_t j = jump_emit(p, kind == EMBI_JST_LAND ? EMBI_JSOP_JMP_FALSE_KEEP
                                                             : EMBI_JSOP_JMP_TRUE_KEEP);
            p->fn->depth -= 1; /* the fall-through pops the operand */
            expr rhs;
            if (!parse_binary(p, prec + 1, no_in, &rhs)) {
                return false;
            }
            to_value(p, &rhs);
            jump_to(p, j, embi_jsc_here(p));
            logical_seen = true;
        } else {
            expr rhs;
            if (!parse_binary(p, op == EMBI_JSOP_EXP ? prec : prec + 1, no_in, &rhs)) {
                return false;
            }
            to_value(p, &rhs);
            embi_jsc_emit_op(p, this_priv_in ? EMBI_JSOP_PRIV_IN : op);
        }
        set_value(&lhs);
    }
    *e = lhs;
    return true;
}


static bool parse_cond(cunit *p, bool no_in, expr *e)
{
    expr c;
    if (!parse_binary(p, 1, no_in, &c)) {
        return false;
    }
    if (!is(p, EMBI_JST_QUESTION)) {
        *e = c;
        return true;
    }
    to_value(p, &c);
    uint32_t jf = jump_emit(p, EMBI_JSOP_JMP_FALSE);
    adv(p);
    int32_t base = p->fn->depth;
    expr a;
    if (!parse_assign(p, false, &a)) {
        return false;
    }
    to_value(p, &a);
    uint32_t je = jump_emit(p, EMBI_JSOP_JMP);
    if (!expect(p, EMBI_JST_COLON)) {
        return false;
    }
    jump_to(p, jf, embi_jsc_here(p));
    p->fn->depth = base;
    expr b;
    if (!parse_assign(p, no_in, &b)) {
        return false;
    }
    to_value(p, &b);
    jump_to(p, je, embi_jsc_here(p));
    set_value(e);
    return true;
}

#define ASSIGN_PLAIN (-2)

static int assign_op(uint8_t k)
{
    switch (k) {
    case EMBI_JST_ASSIGN:
        return ASSIGN_PLAIN;
    case EMBI_JST_ADD_EQ:
        return EMBI_JSOP_ADD;
    case EMBI_JST_SUB_EQ:
        return EMBI_JSOP_SUB;
    case EMBI_JST_MUL_EQ:
        return EMBI_JSOP_MUL;
    case EMBI_JST_DIV_EQ:
        return EMBI_JSOP_DIV;
    case EMBI_JST_MOD_EQ:
        return EMBI_JSOP_MOD;
    case EMBI_JST_EXP_EQ:
        return EMBI_JSOP_EXP;
    case EMBI_JST_SHL_EQ:
        return EMBI_JSOP_SHL;
    case EMBI_JST_SAR_EQ:
        return EMBI_JSOP_SAR;
    case EMBI_JST_SHR_EQ:
        return EMBI_JSOP_SHR;
    case EMBI_JST_BAND_EQ:
        return EMBI_JSOP_BAND;
    case EMBI_JST_BOR_EQ:
        return EMBI_JSOP_BOR;
    case EMBI_JST_BXOR_EQ:
        return EMBI_JSOP_BXOR;
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ destructuring patterns */

/*
 * Binding and assignment patterns (ES2015 13.15.5, 14.3.3, 14.15.2). The parser emits code in
 * source order, but a pattern takes its value from a source that comes after it: the initializer of
 * a declaration or an assignment, the default of a parameter, or the right-hand side of a for-in
 * head. pat_layout lays such a pattern out behind a jump so that the source code runs first:
 *
 *       JMP R              over the pattern
 *   P:  GET T              the value (T is a temporary that the source code fills)
 *       <pattern>          consumes the value
 *       JMP Q
 *   R:  <source>           leaves the value on the stack
 *       INIT T
 *       JMP P
 *   Q:  [GET T]            the value of an assignment expression
 *
 * A for-in head uses the replay layout of parse_for instead: the pattern runs with the key on the
 * stack. An array pattern keeps its iterator record in a temporary. Its protected range closes the
 * iterator on a throw (ITER_CLOSE_THROW, then RETHROW), and a normal exit closes it after the last
 * element. An object pattern keeps its source, and the keys of a rest element, in temporaries.
 *
 * The lookahead (pscan) scans a bracket group without emitting code. It tells a pattern from a
 * literal by the token after the group, and pat_declare_names declares the names of a binding
 * pattern before any default is parsed, so that a default can refer to a later name in its temporal
 * dead zone.
 */

/* What follows a bracket group (group_follow). */
enum { GF_OTHER, GF_ASSIGN, GF_ELEM, GF_IN, GF_OF };

/* A pattern. A binding pattern declares its names with the binding kind in scope; an assignment
 * pattern assigns to references. */
typedef struct pat {
    bool bind;
    uint8_t kind;   /* BK_VAR, BK_LET, BK_CONST, BK_PARAM or BK_CATCH */
    bool head;      /* a for head: its lexical names start uninitialized */
    cscope *scope;  /* a binding pattern: the scope that holds the names */
} pat;

/* Why a lookahead failed (pscan.fail). A failure with neither flag is a mismatch, which the caller
 * reports as an unexpected token. */
enum { SCAN_NONE, SCAN_ERROR, SCAN_DEEP };

/* A lookahead over a copy of the lexer. The open brackets take two bits per level (pscan_push), so
 * the stack has room for every level the parser allows (EMBI_JS_PARSE_MAX_NEST). */
typedef struct pscan {
    cunit *p;
    embi_jslex lx;
    embi_jstok t;
    bool operand;      /* the previous token ends an operand, so a '/' is a division */
    uint32_t sp;
    int fail;          /* SCAN_ERROR: the parser has reported an error; SCAN_DEEP: too deeply nested */
    uint8_t open[EMBI_JS_PARSE_MAX_NEST / 4 + 1]; /* '(', '[', '{', or 'T' (a template substitution) */
} pscan;

static void pscan_init(pscan *s, cunit *p)
{
    s->p = p;
    s->lx = p->lx;
    s->lx.scratch = p->peek_scratch;
    s->lx.scratch_cap = p->peek_cap;
    s->t = p->tok;
    s->operand = false;
    s->sp = 0;
    s->fail = SCAN_NONE;
}

static void pscan_push(pscan *s, char c)
{
    uint32_t code = c == '(' ? 0u : c == '[' ? 1u : c == '{' ? 2u : 3u;
    uint32_t i = s->sp++;
    uint32_t shift = (i & 3u) * 2u;
    s->open[i >> 2] = (uint8_t)((s->open[i >> 2] & ~(3u << shift)) | (code << shift));
}

static char pscan_at(const pscan *s, uint32_t i)
{
    static const char chars[4] = {'(', '[', '{', 'T'};
    return chars[(s->open[i >> 2] >> ((i & 3u) * 2u)) & 3u];
}

static char pscan_top(const pscan *s)
{
    return pscan_at(s, s->sp - 1);
}

/* Opens a bracket level. Answers false when the nesting is past the limit (SCAN_DEEP). */
static bool scan_open(pscan *s, char c)
{
    if (s->sp == EMBI_JS_PARSE_MAX_NEST) {
        s->fail = SCAN_DEEP;
        return false;
    }
    cunit *p = s->p;
    while (p->gstk.len <= s->sp) {
        cgframe z = {UINT32_MAX, false};
        embi_vec_push(&p->gstk, z);
    }
    p->gstk.data[s->sp].start = c == '[' || c == '{' ? s->t.start : UINT32_MAX;
    p->gstk.data[s->sp].rest = false;
    pscan_push(s, c);
    return true;
}

static char open_char(embi_jstok_kind k)
{
    return k == EMBI_JST_LPAREN ? '(' : k == EMBI_JST_LBRACK ? '[' : '{';
}

static bool scan_ends_operand(const embi_jstok *t)
{
    switch (t->kind) {
    case EMBI_JST_NAME:
        return t->kw == EMBI_JSKW_NONE || t->kw == EMBI_JSKW_this || t->kw == EMBI_JSKW_null ||
               t->kw == EMBI_JSKW_true || t->kw == EMBI_JSKW_false;
    case EMBI_JST_NUMBER:
    case EMBI_JST_BIGINT:
    case EMBI_JST_STRING:
    case EMBI_JST_REGEX:
    case EMBI_JST_TPL_NOSUB:
    case EMBI_JST_TPL_TAIL:
    case EMBI_JST_RPAREN:
    case EMBI_JST_RBRACK:
    case EMBI_JST_RBRACE:
    case EMBI_JST_INC:
    case EMBI_JST_DEC:
        return true;
    default:
        return false;
    }
}

/* Reads the token after s->t. A '}' that closes a template substitution reads the rest of the
 * template. */
static void pscan_next(pscan *s)
{
    s->lx.regex_ok = !s->operand;
    lex_scan(s->p, &s->lx, &s->t, true);
    if (s->t.kind == EMBI_JST_RBRACE && s->sp > 0 && pscan_top(s) == 'T') {
        embi_jslex_rescan_template(&s->lx, &s->t);
        if (s->t.kind == EMBI_JST_TPL_TAIL) {
            s->sp--;
        }
    }
    s->operand = scan_ends_operand(&s->t);
}

static bool scan_closes(embi_jstok_kind k, uint8_t open)
{
    return (k == EMBI_JST_RPAREN && open == '(') || (k == EMBI_JST_RBRACK && open == '[') ||
           (k == EMBI_JST_RBRACE && open == '{');
}

/* The follow kind of a group that the token t comes after. */
static int follow_kind(const cunit *p, const embi_jstok *t)
{
    switch (t->kind) {
    case EMBI_JST_ASSIGN:
        return GF_ASSIGN;
    case EMBI_JST_COMMA:
    case EMBI_JST_RBRACK:
    case EMBI_JST_RBRACE:
    case EMBI_JST_RPAREN:
        return GF_ELEM;
    case EMBI_JST_NAME:
        if (t->flags & EMBI_JST_F_ESCAPED) {
            return GF_OTHER;
        }
        if (t->kw == EMBI_JSKW_in) {
            return GF_IN;
        }
        if (t->end - t->start == 2 && memcmp(p->src + t->start, "of", 2) == 0) {
            return GF_OF;
        }
        return GF_OTHER;
    default:
        return GF_OTHER;
    }
}

/* Memoizes a closed group (frame fr, already popped) once s->t is the token after its closer. Every
 * group a scan passes is recorded, so the parser's later lookups at the inner groups are table hits
 * and the lookahead work over a whole literal is linear in its size, however deeply it nests. */
static void scan_record(pscan *s, cgframe fr)
{
    /* embi_hashmap_put reports insert-versus-replace, not failure: allocation failure is fatal in
     * base, so there is no out-of-memory path to propagate here. */
    if (fr.start != UINT32_MAX) {
        (void)embi_hashmap_put(&s->p->grp, fr.start, (uint64_t)follow_kind(s->p, &s->t) | (fr.rest ? 256u : 0u));
    }
}

/* Skips tokens at the level where the scan starts, until a closer of that level or, when comma is
 * set, a ',' of that level (neither is consumed). rest is set when an ellipsis of that level is
 * seen. Answers false at the end of input, an error, or a mismatched bracket. */
static bool scan_until(pscan *s, bool comma)
{
    uint32_t base = s->sp;
    for (;;) {
        switch (s->t.kind) {
        case EMBI_JST_EOF:
        case EMBI_JST_ERROR:
            return false;
        case EMBI_JST_LPAREN:
        case EMBI_JST_LBRACK:
        case EMBI_JST_LBRACE:
            if (!scan_open(s, open_char(s->t.kind))) {
                return false;
            }
            break;
        case EMBI_JST_TPL_HEAD:
            if (!scan_open(s, 'T')) {
                return false;
            }
            break;
        case EMBI_JST_RPAREN:
        case EMBI_JST_RBRACK:
        case EMBI_JST_RBRACE:
            if (s->sp == base) {
                return true;
            }
            if (!scan_closes(s->t.kind, pscan_top(s))) {
                return false;
            }
            {
                cgframe fr = s->p->gstk.data[s->sp - 1];
                s->sp--;
                pscan_next(s);
                scan_record(s, fr);
            }
            continue;
        case EMBI_JST_COMMA:
            if (comma && s->sp == base) {
                return true;
            }
            break;
        case EMBI_JST_ELLIPSIS:
            if (s->sp > 0) {
                s->p->gstk.data[s->sp - 1].rest = true;
            }
            break;
        default:
            break;
        }
        pscan_next(s);
    }
}

/* The bracket group at s->t (an opener) is skipped: s->t is left at the token after its closer. */
static bool scan_group(pscan *s)
{
    uint32_t base = s->sp;
    if (!scan_open(s, open_char(s->t.kind))) {
        return false;
    }
    pscan_next(s);
    if (!scan_until(s, false)) {
        return false;
    }
    if (s->sp != base + 1 || !scan_closes(s->t.kind, pscan_at(s, base))) {
        return false;
    }
    cgframe fr = s->p->gstk.data[base];
    s->sp = base;
    pscan_next(s);
    scan_record(s, fr);
    return true;
}

/* The memo entry of the bracket group at the current token, scanning it first when absent. Answers
 * false for a group that does not scan (unbalanced or too deep). */
static bool group_info(cunit *p, int *follow, bool *rest)
{
    uint64_t v;
    if (!embi_hashmap_get(&p->grp, p->tok.start, &v)) {
        pscan s;
        pscan_init(&s, p);
        if (!scan_group(&s) || !embi_hashmap_get(&p->grp, p->tok.start, &v)) {
            return false;
        }
    }
    *follow = (int)(v & 255u);
    *rest = (v & 256u) != 0;
    return true;
}

/* What follows the bracket group at the current token. */
static int group_follow(cunit *p)
{
    int f;
    bool r;
    return group_info(p, &f, &r) ? f : GF_OTHER;
}

/* Whether the bracket group at the current token has a rest element at its top level. */
static bool group_has_rest(cunit *p)
{
    int f;
    bool r;
    return group_info(p, &f, &r) && r;
}

static bool scan_is_ident(const pscan *s)
{
    if (s->t.kind != EMBI_JST_NAME) {
        return false;
    }
    if (s->t.kw == EMBI_JSKW_NONE) {
        return true;
    }
    if (s->t.kw == EMBI_JSKW_await && await_reserved(s->p)) {
        return false;
    }
    if (reserved_kw(s->t.kw)) {
        return false;
    }
    return !(s->p->fn->strict && strict_reserved_kw(s->t.kw));
}

/* The atom of a NAME token t (not necessarily the current token of the parser). */
static embi_js_value tok_name(cunit *p, const embi_jstok *t)
{
    if (t->flags & EMBI_JST_F_ESCAPED) {
        embi_js_string *s = embi_js_str_new_utf16(p->rt, t->cooked, t->cooked_len);
        return embi_js_from_string(embi_js_atom_intern(p->rt, s));
    }
    return embi_js_from_string(
        embi_js_atom_intern_utf8(p->rt, (const char *)p->src + t->start, t->end - t->start));
}

/* Declares one name of a binding pattern. */
static bool pat_declare(cunit *p, const pat *c, embi_js_value nm)
{
    cfunc *f = p->fn;
    if (!check_strict_name(p, nm)) {
        return false;
    }
    switch (c->kind) {
    case BK_VAR:
        if (!check_var_conflict(p, nm)) {
            return false;
        }
        if (p->eval_params && f->is_eval && !f->strict && nm == p->atom_arguments) {
            return perr(p, "Identifier 'arguments' cannot be declared by eval in a parameter default");
        }
        note_var_name(p, nm);
        declare_var(p, nm, BK_VAR);
        return true;
    case BK_LET:
    case BK_CONST: {
        if (nm == p->atom_let) {
            return perr(p, "let is disallowed as a lexically bound name");
        }
        bool ok;
        cbind *b = declare_lexical(p, nm, c->kind, &ok);
        if (!ok) {
            return false;
        }
        if (c->head) {
            OPC(PUSH_EMPTY);
            embi_jsc_emit_sym(p, SYM_KIND_INIT, nm, f->cur, b);
        }
        return true;
    }
    case BK_PARAM:
        declare_param(p, nm, 0);
        return true;
    default: /* BK_CATCH */
        if (embi_jsc_find_in_scope(c->scope, nm)) {
            return perr(p, "Identifier has already been declared");
        }
        embi_jsc_new_bind(p, f, c->scope, nm, BK_CATCH)->catch_pat = true;
        return true;
    }
}

static bool scan_pattern(pscan *s, const pat *c);

/* Declares a name while scanning. A failure is an error that the parser has already reported. */
static bool scan_declare(pscan *s, const pat *c, embi_js_value nm)
{
    if (!pat_declare(s->p, c, nm)) {
        s->fail = SCAN_ERROR;
        return false;
    }
    return true;
}

/* Skips an initializer when the current token is '='. */
static bool scan_init(pscan *s)
{
    if (s->t.kind != EMBI_JST_ASSIGN) {
        return true;
    }
    pscan_next(s);
    return scan_until(s, true);
}

/* One target of a binding pattern: a nested pattern or a name, then its initializer. */
static bool scan_target(pscan *s, const pat *c)
{
    if (s->t.kind == EMBI_JST_LBRACK || s->t.kind == EMBI_JST_LBRACE) {
        if (!scan_pattern(s, c)) {
            return false;
        }
    } else if (scan_is_ident(s)) {
        embi_js_value nm = tok_name(s->p, &s->t);
        if (!scan_declare(s, c, nm)) {
            return false;
        }
        pscan_next(s);
    } else {
        return false;
    }
    return scan_init(s);
}

/* A binding pattern at s->t ('[' or '{'): declares its names. s->t is left after the pattern. */
static bool scan_pattern_body(pscan *s, const pat *c)
{
    bool obj = s->t.kind == EMBI_JST_LBRACE;
    embi_jstok_kind close = obj ? EMBI_JST_RBRACE : EMBI_JST_RBRACK;
    pscan_next(s);
    while (s->t.kind != close) {
        if (s->t.kind == EMBI_JST_COMMA) {
            pscan_next(s);
            continue;
        }
        if (s->t.kind == EMBI_JST_EOF || s->t.kind == EMBI_JST_ERROR) {
            return false;
        }
        if (s->t.kind == EMBI_JST_ELLIPSIS) {
            pscan_next(s);
            if (obj) {
                if (!scan_is_ident(s)) {
                    return false;
                }
                embi_js_value nm = tok_name(s->p, &s->t);
                if (!scan_declare(s, c, nm)) {
                    return false;
                }
                pscan_next(s);
            } else if (!scan_target(s, c)) {
                return false;
            }
        } else if (obj) {
            if (s->t.kind == EMBI_JST_LBRACK) {
                if (!scan_group(s) || s->t.kind != EMBI_JST_COLON) {
                    return false;
                }
                pscan_next(s);
                if (!scan_target(s, c)) {
                    return false;
                }
            } else if (s->t.kind == EMBI_JST_NAME || s->t.kind == EMBI_JST_STRING ||
                       s->t.kind == EMBI_JST_NUMBER || s->t.kind == EMBI_JST_BIGINT) {
                bool name = scan_is_ident(s);
                embi_js_value nm = name ? tok_name(s->p, &s->t) : EMBI_JS_EMPTY;
                pscan_next(s);
                if (s->t.kind == EMBI_JST_COLON) {
                    pscan_next(s);
                    if (!scan_target(s, c)) {
                        return false;
                    }
                } else if (!name || !scan_declare(s, c, nm) || !scan_init(s)) {
                    return false;
                }
            } else {
                return false;
            }
        } else if (!scan_target(s, c)) {
            return false;
        }
        if (s->t.kind == EMBI_JST_COMMA) {
            pscan_next(s);
        } else if (s->t.kind != close) {
            return false;
        }
    }
    pscan_next(s);
    return true;
}

/* scan_pattern_body under the nesting guard. The scan recurses before the parser's own guards
 * (pat_array, pat_object) run, so it takes one itself: a deeply nested pattern is a RangeError
 * whichever declaration form holds it. */
static bool scan_pattern(pscan *s, const pat *c)
{
    if (!nest_enter(s->p)) {
        s->p->nest--;
        s->fail = SCAN_ERROR;
        return false;
    }
    bool ok = scan_pattern_body(s, c);
    s->p->nest--;
    return ok;
}

/* Declares the names of the binding pattern at the current token (nothing for an assignment
 * pattern). */
static bool pat_declare_names(cunit *p, const pat *c)
{
    if (!c->bind) {
        return true;
    }
    pscan s;
    pscan_init(&s, p);
    if (!scan_pattern(&s, c)) {
        if (s.fail == SCAN_ERROR) {
            return false; /* the declaration's own message, such as a redeclaration */
        }
        if (s.fail == SCAN_DEEP) {
            return perr_plain(p, EMBI_JS_PROTO_RANGE_ERROR, "Maximum nesting depth exceeded");
        }
        return unexpected(p);
    }
    return true;
}

/* A pattern element at the current token: a binding pattern always, and in an assignment a
 * bracket group that is followed by a separator or '=' (otherwise a member of a literal target,
 * such as [a][0]). */
static bool elem_is_pattern(cunit *p, const pat *c)
{
    if (!is(p, EMBI_JST_LBRACK) && !is(p, EMBI_JST_LBRACE)) {
        return false;
    }
    if (c->bind) {
        return true;
    }
    int gf = group_follow(p);
    return gf == GF_ASSIGN || gf == GF_ELEM;
}

/* An assignment target: a reference, not loaded yet (its base and key are on the stack). */
static bool pat_target(cunit *p, expr *t)
{
    if (!parse_lhs_ex(p, t, true)) {
        return false;
    }
    if (!assignable(t)) {
        return perr(p, "Invalid destructuring assignment target");
    }
    return check_target(p, t);
}

static cbind *pat_temp(cunit *p)
{
    return embi_jsc_new_bind(p, p->fn, nullptr, EMBI_JS_EMPTY, BK_TEMP);
}

static void tmp_get(cunit *p, cbind *t)
{
    embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t);
}

static void tmp_init(cunit *p, cbind *t)
{
    embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, t);
}

/* Whether the name of a var binding in a pattern is resolved before its value is fetched: the
 * name may cross a with object (BindingInitialization resolves the binding first). */
static bool bind_early(const cunit *p, const pat *c)
{
    if (c->kind != BK_VAR) {
        return false;
    }
    if (p->fn->dynref) {
        return true;
    }
    for (const cscope *s = p->fn->cur; s; s = s->parent) {
        if (s->kind == SC_WITH) {
            return true;
        }
    }
    return false;
}

/* Stores the value on the stack into a name of a binding pattern (the value is consumed). */
static void pat_store(cunit *p, const pat *c, embi_js_value nm)
{
    if (c->kind == BK_VAR) {
        embi_jsc_emit_sym(p, bind_early(p, c) ? SYM_KIND_SETREF : SYM_KIND_SET, nm, p->fn->cur,
                          nullptr);
        OPC(POP);
        return;
    }
    cbind *b = embi_jsc_find_in_scope(c->scope, nm);
    /* Every reference is checked while the declaration is parsed: the pattern code precedes its
     * source expression in the code, so the INIT's pc says nothing about the source. In a for head
     * it stays checked (the head expression runs before the first iteration binds the name);
     * otherwise parse_pattern_decl lowers it once the whole declaration is laid out. */
    b->init_pc = 0xFFFFFFFFu;
    embi_jsc_emit_sym(p, SYM_KIND_INIT, nm, c->scope, b);
}

/* Stores the value on the stack into an assignment target (the value is consumed). */
static void pat_store_ref(cunit *p, const expr *t, bool early)
{
    if (early) {
        embi_jsc_emit_sym(p, SYM_KIND_SETREF, t->name, t->scope, nullptr);
    } else {
        ref_set(p, t);
    }
    OPC(POP);
}

/* An initializer applies to a value that is undefined. */
static bool pat_default(cunit *p, embi_js_value hint)
{
    if (!is(p, EMBI_JST_ASSIGN)) {
        return true;
    }
    return parse_param_default(p, hint);
}

static bool pat_array(cunit *p, const pat *c);
static bool pat_object(cunit *p, const pat *c);

/* A pattern that takes the value on the stack. */
static bool pat_consume(cunit *p, const pat *c)
{
    if (is(p, EMBI_JST_LBRACK)) {
        return pat_array(p, c);
    }
    if (is(p, EMBI_JST_LBRACE)) {
        return pat_object(p, c);
    }
    return unexpected(p);
}

/* A nested pattern element that takes the value on the stack. Its initializer follows the pattern
 * in the source, so the pattern code is laid out first and the default behind a jump, as in
 * pat_layout: the default applies when the value is undefined, then the value is destructured. */
static bool pat_nested(cunit *p, const pat *c)
{
    if (group_follow(p) != GF_ASSIGN) {
        return pat_consume(p, c);
    }
    cbind *v = pat_temp(p);
    tmp_init(p, v);
    uint32_t j_src = jump_emit(p, EMBI_JSOP_JMP);
    uint32_t pat_pc = embi_jsc_here(p);
    tmp_get(p, v);
    if (!pat_consume(p, c)) {
        return false;
    }
    uint32_t j_done = jump_emit(p, EMBI_JSOP_JMP);
    jump_to(p, j_src, embi_jsc_here(p));
    tmp_get(p, v);
    if (!parse_param_default(p, EMBI_JS_EMPTY)) {
        return false;
    }
    tmp_init(p, v);
    OPC32(JMP, pat_pc);
    jump_to(p, j_done, embi_jsc_here(p));
    return true;
}

/* Pushes the next value of an array pattern's record. */
static void rec_value(cunit *p, cbind *rec)
{
    tmp_get(p, rec);
    OPC(ITER_VALUE);
}

/* An element of an array pattern (neither a hole nor the rest element). */
static bool pat_elem(cunit *p, const pat *c, cbind *rec)
{
    if (elem_is_pattern(p, c)) {
        rec_value(p, rec);
        return pat_nested(p, c);
    }
    if (c->bind) {
        if (!is_ident(p)) {
            return unexpected(p);
        }
        embi_js_value nm = name_atom(p);
        adv(p);
        if (bind_early(p, c)) {
            embi_jsc_emit_sym(p, SYM_KIND_REF, nm, p->fn->cur, nullptr);
        }
        rec_value(p, rec);
        if (!pat_default(p, nm)) {
            return false;
        }
        pat_store(p, c, nm);
        return true;
    }
    expr t;
    if (!pat_target(p, &t)) {
        return false;
    }
    bool early = early_ref(p, &t);
    if (early) {
        embi_jsc_emit_sym(p, SYM_KIND_REF, t.name, t.scope, nullptr);
    }
    rec_value(p, rec);
    if (!pat_default(p, t.kind == EX_VAR && !t.paren ? t.name : EMBI_JS_EMPTY)) {
        return false;
    }
    pat_store_ref(p, &t, early);
    return true;
}

/* The rest element of an array pattern (after its '...'). */
static bool pat_rest_elem(cunit *p, const pat *c, cbind *rec)
{
    if (elem_is_pattern(p, c)) {
        tmp_get(p, rec);
        OPC(ITER_REST);
        if (!pat_consume(p, c)) {
            return false;
        }
    } else if (c->bind) {
        if (!is_ident(p)) {
            return unexpected(p);
        }
        embi_js_value nm = name_atom(p);
        adv(p);
        if (bind_early(p, c)) {
            embi_jsc_emit_sym(p, SYM_KIND_REF, nm, p->fn->cur, nullptr);
        }
        tmp_get(p, rec);
        OPC(ITER_REST);
        pat_store(p, c, nm);
    } else {
        expr t;
        if (!pat_target(p, &t)) {
            return false;
        }
        bool early = early_ref(p, &t);
        if (early) {
            embi_jsc_emit_sym(p, SYM_KIND_REF, t.name, t.scope, nullptr);
        }
        tmp_get(p, rec);
        OPC(ITER_REST);
        pat_store_ref(p, &t, early);
    }
    if (is(p, EMBI_JST_ASSIGN)) {
        return perr(p, "Rest element may not have a default initializer");
    }
    return true;
}

/* [ pattern ]: consumes the value on the stack. */
static bool pat_array(cunit *p, const pat *c)
{
    NEST_GUARD(p);
    cfunc *f = p->fn;
    adv(p); /* [ */
    cbind *rec = pat_temp(p);
    OPC(ITER_INIT);
    tmp_init(p, rec);
    int32_t depth = f->depth;
    uint32_t start = embi_jsc_here(p);
    /* The elements run with the record on the control stack, so a return completion from a yield
     * inside them closes it (J3e). */
    ctl pc = {nullptr, CTL_PATTERN, 0, 0, {0}, nullptr, nullptr, nullptr, nullptr, 2,
              nullptr, nullptr, false, false, nullptr};
    pc.parent = p->ctl;
    pc.pat_rec = rec;
    p->ctl = &pc;
    while (!is(p, EMBI_JST_RBRACK)) {
        if (is(p, EMBI_JST_COMMA)) {
            rec_value(p, rec);
            OPC(POP);
            adv(p);
            continue;
        }
        if (is(p, EMBI_JST_ELLIPSIS)) {
            adv(p);
            if (!pat_rest_elem(p, c, rec)) {
                return false;
            }
            if (!is(p, EMBI_JST_RBRACK)) {
                return perr(p, "Rest element must be last element");
            }
            break;
        }
        if (!pat_elem(p, c, rec)) {
            return false;
        }
        if (is(p, EMBI_JST_COMMA)) {
            adv(p);
        } else if (!is(p, EMBI_JST_RBRACK)) {
            return unexpected(p);
        }
    }
    p->ctl = pc.parent;
    uint32_t end = embi_jsc_here(p);
    adv(p); /* ] */
    tmp_get(p, rec);
    OPC(ITER_CLOSE);
    uint32_t j_over = jump_emit(p, EMBI_JSOP_JMP);
    /* A throw inside the elements closes the iterator (errors of return dropped), then rethrows
     * the exception the runtime pushed at the base depth of the pattern. */
    uint32_t handler = embi_jsc_here(p);
    f->depth = depth + 1;
    tmp_get(p, rec);
    OPC(ITER_CLOSE_THROW);
    OPC(RETHROW);
    f->depth = depth;
    jump_to(p, j_over, embi_jsc_here(p));
    embi_js_exc_entry ent = {start, end, handler, (uint32_t)depth, 0};
    CF_PUSH(f, exc, ent);
    return true;
}

/* Fetches a property of the object temporary src: the key is a temporary (computed) or a
 * constant. */
static void pat_fetch(cunit *p, cbind *src, cbind *key, uint32_t kc)
{
    tmp_get(p, src);
    if (key) {
        tmp_get(p, key);
        OPC(GET_INDEX);
    } else {
        OPC32(GET_FIELD, kc);
    }
}

/* The rest element of an object pattern (after its '...'). */
static bool pat_obj_rest(cunit *p, const pat *c, cbind *src, cbind *excl)
{
    if (!excl) {
        return unexpected(p);
    }
    if (c->bind) {
        if (!is_ident(p)) {
            return unexpected(p);
        }
        embi_js_value nm = name_atom(p);
        adv(p);
        if (bind_early(p, c)) {
            embi_jsc_emit_sym(p, SYM_KIND_REF, nm, p->fn->cur, nullptr);
        }
        tmp_get(p, src);
        tmp_get(p, excl);
        OPC(OBJ_REST);
        pat_store(p, c, nm);
    } else {
        expr t;
        if (!pat_target(p, &t)) {
            return false;
        }
        bool early = early_ref(p, &t);
        if (early) {
            embi_jsc_emit_sym(p, SYM_KIND_REF, t.name, t.scope, nullptr);
        }
        tmp_get(p, src);
        tmp_get(p, excl);
        OPC(OBJ_REST);
        pat_store_ref(p, &t, early);
    }
    if (is(p, EMBI_JST_ASSIGN)) {
        return perr(p, "Rest element may not have a default initializer");
    }
    return true;
}

/* One property of an object pattern: its key, then a target, or a shorthand name. A computed key
 * is converted once (into a temporary) before the target is evaluated. */
static bool pat_prop(cunit *p, const pat *c, cbind *src, cbind *excl)
{
    cfunc *f = p->fn;
    bool computed = is(p, EMBI_JST_LBRACK);
    cbind *key = nullptr;
    embi_js_value kname = EMBI_JS_EMPTY;
    bool ident_key = false;
    uint32_t kc = 0;
    if (computed) {
        adv(p);
        expr k;
        if (!parse_assign(p, false, &k)) {
            return false;
        }
        to_value(p, &k);
        if (!expect(p, EMBI_JST_RBRACK)) {
            return false;
        }
        OPC(TO_PROPKEY);
        key = pat_temp(p);
        tmp_init(p, key);
        if (excl) {
            tmp_get(p, excl);
            tmp_get(p, key);
            OPC(ARRAY_PUSH);
            OPC(POP);
        }
    } else {
        ident_key = is_ident(p);
        if (!property_key(p, &kname)) {
            return false;
        }
        adv(p);
        kc = const_str(p, kname);
        if (excl) {
            tmp_get(p, excl);
            OPC32(PUSH_CONST, kc);
            OPC(ARRAY_PUSH);
            OPC(POP);
        }
    }
    if (is(p, EMBI_JST_COLON)) {
        adv(p);
        if (elem_is_pattern(p, c)) {
            pat_fetch(p, src, key, kc);
            return pat_nested(p, c);
        }
        if (c->bind) {
            if (!is_ident(p)) {
                return unexpected(p);
            }
            embi_js_value nm = name_atom(p);
            adv(p);
            if (bind_early(p, c)) {
                embi_jsc_emit_sym(p, SYM_KIND_REF, nm, p->fn->cur, nullptr);
            }
            pat_fetch(p, src, key, kc);
            if (!pat_default(p, nm)) {
                return false;
            }
            pat_store(p, c, nm);
            return true;
        }
        expr t;
        if (!pat_target(p, &t)) {
            return false;
        }
        bool early = early_ref(p, &t);
        if (early) {
            embi_jsc_emit_sym(p, SYM_KIND_REF, t.name, t.scope, nullptr);
        }
        pat_fetch(p, src, key, kc);
        if (!pat_default(p, t.kind == EX_VAR && !t.paren ? t.name : EMBI_JS_EMPTY)) {
            return false;
        }
        pat_store_ref(p, &t, early);
        return true;
    }
    /* Shorthand: the key is the name of the target. */
    if (computed || !ident_key) {
        return unexpected(p);
    }
    if (c->bind) {
        if (bind_early(p, c)) {
            embi_jsc_emit_sym(p, SYM_KIND_REF, kname, p->fn->cur, nullptr);
        }
        pat_fetch(p, src, nullptr, kc);
        if (!pat_default(p, kname)) {
            return false;
        }
        pat_store(p, c, kname);
        return true;
    }
    expr t;
    memset(&t, 0, sizeof t);
    t.kind = EX_VAR;
    t.name = kname;
    t.scope = f->cur;
    if (!check_target(p, &t)) {
        return false;
    }
    bool early = early_ref(p, &t);
    if (early) {
        embi_jsc_emit_sym(p, SYM_KIND_REF, t.name, t.scope, nullptr);
    }
    pat_fetch(p, src, nullptr, kc);
    if (!pat_default(p, kname)) {
        return false;
    }
    pat_store_ref(p, &t, early);
    return true;
}

/* { pattern }: consumes the value on the stack. */
static bool pat_object(cunit *p, const pat *c)
{
    NEST_GUARD(p);
    bool has_rest = group_has_rest(p);
    adv(p); /* { */
    OPC(REQ_COERCIBLE);
    cbind *src = pat_temp(p);
    tmp_init(p, src);
    cbind *excl = nullptr;
    if (has_rest) {
        OPC32(NEW_ARRAY, 0);
        excl = pat_temp(p);
        tmp_init(p, excl);
    }
    while (!is(p, EMBI_JST_RBRACE)) {
        if (is(p, EMBI_JST_ELLIPSIS)) {
            adv(p);
            if (!pat_obj_rest(p, c, src, excl)) {
                return false;
            }
            if (!is(p, EMBI_JST_RBRACE)) {
                return perr(p, "Rest element must be last element");
            }
            break;
        }
        if (!pat_prop(p, c, src, excl)) {
            return false;
        }
        if (is(p, EMBI_JST_COMMA)) {
            adv(p);
        } else if (!is(p, EMBI_JST_RBRACE)) {
            return unexpected(p);
        }
    }
    adv(p); /* } */
    return true;
}

#define PS_RHS 0 /* the source is the initializer that follows the '=' */
#define PS_ARG 1 /* the source is argument slot arg; its default is parsed here */

/* A pattern laid out before its source (see the comment above). When result is set, the value of
 * the source stays on the stack (an assignment expression). */
static bool pat_layout(cunit *p, const pat *c, int src, uint32_t arg, bool no_in, bool result)
{
    cbind *t = pat_temp(p);
    uint32_t j_src = jump_emit(p, EMBI_JSOP_JMP);
    uint32_t pat_pc = embi_jsc_here(p);
    tmp_get(p, t);
    if (!pat_consume(p, c)) {
        return false;
    }
    uint32_t j_done = jump_emit(p, EMBI_JSOP_JMP);
    jump_to(p, j_src, embi_jsc_here(p));
    if (src == PS_ARG) {
        OPC32(GET_LOC, arg);
        if (!parse_param_default(p, EMBI_JS_EMPTY)) {
            return false;
        }
    } else {
        if (!is(p, EMBI_JST_ASSIGN)) {
            return perr(p, "Missing initializer in destructuring declaration");
        }
        adv(p);
        expr e;
        if (!parse_assign(p, no_in, &e)) {
            return false;
        }
        to_value(p, &e);
    }
    tmp_init(p, t);
    OPC32(JMP, pat_pc);
    jump_to(p, j_done, embi_jsc_here(p));
    if (result) {
        tmp_get(p, t);
    }
    return true;
}

/* A binding pattern of a declaration with its initializer (var, let or const). */
static bool parse_pattern_decl(cunit *p, const pat *c, bool no_in)
{
    if (!pat_declare_names(p, c)) {
        return false;
    }
    if (!pat_layout(p, c, PS_RHS, 0, no_in, false)) {
        return false;
    }
    if (!c->head && (c->kind == BK_LET || c->kind == BK_CONST) && c->scope) {
        /* Straight-line code resumes here, after every name of the pattern is initialized. */
        uint32_t done = embi_jsc_here(p) - 1u;
        for (cbind *b = c->scope->first; b; b = b->next_in_scope) {
            if ((b->kind == BK_LET || b->kind == BK_CONST) && b->init_pc == 0xFFFFFFFFu) {
                b->init_pc = done;
            }
        }
    }
    return true;
}

/* A parameter pattern at argument slot n (rest or not). Its default applies to the argument. */
static bool parse_param_pattern(cunit *p, cfunc *f, uint32_t n, bool rest)
{
    pat c = {true, BK_PARAM, false, f->root};
    if (!pat_declare_names(p, &c)) {
        return false;
    }
    if (!rest && group_follow(p) == GF_ASSIGN) {
        return pat_layout(p, &c, PS_ARG, n, false, false);
    }
    OPC32(GET_LOC, n);
    return pat_consume(p, &c);
}

/* An assignment pattern as an expression: its value is the value of the right-hand side. */
static bool parse_assign_pattern(cunit *p, bool no_in, expr *e)
{
    NEST_GUARD(p);
    p->hint = EMBI_JS_EMPTY;
    pat c = {false, 0, false, nullptr};
    if (!pat_layout(p, &c, PS_RHS, 0, no_in, true)) {
        return false;
    }
    set_value(e);
    return true;
}

/* Whether the current async token starts an async arrow function: `async x =>` or `async (...) =>`
 * with no line break after async (the parameters are scanned on a copy of the lexer). */
static bool async_arrow_ahead(cunit *p)
{
    embi_jslex lx = p->lx;
    lx.scratch = p->peek_scratch;
    lx.scratch_cap = p->peek_cap;
    embi_jstok t;
    lex_scan(p, &lx, &t, true);
    if (t.flags & EMBI_JST_F_NL_BEFORE) {
        return false;
    }
    if (t.kind == EMBI_JST_NAME && t.kw != EMBI_JSKW_function) {
        lex_scan(p, &lx, &t, true);
        return t.kind == EMBI_JST_ARROW && !(t.flags & EMBI_JST_F_NL_BEFORE);
    }
    if (t.kind == EMBI_JST_LPAREN) {
        return arrow_ahead_lx(p, lx, t.start);
    }
    return false;
}

/* A yield expression (ES2015 14.4). yield without a delegate: YIELD suspends the generator, and
 * GEN_RESUME takes the received value. A return resumption jumps to a return statement that runs
 * the finally blocks around the yield. yield* loops in DELEGATE_STEP and YIELD_RAW: each pass
 * forwards the received value and mode to the inner iterator, and yields its result object as is.
 * DELEGATE_STEP leaves [value] and jumps to the normal exit, or to the return path for a return
 * completion. */
/* yield* in an async generator (14.4.14, J3b). The operand is on the stack; the loop keeps
 * [rec received mode] (ASYNC_DELEGATE_CALL, AWAIT, ASYNC_DELEGATE_CHECK, ASYNC_YIELD). The
 * missing-method paths close the iterator (AWAIT on its return result) and then throw a TypeError
 * or return the received value. Leaves the value of the expression on the stack. */
static void async_delegate(cunit *p) {
    int32_t entry = p->fn->depth;
    OPC(ASYNC_ITER_INIT);
    OPC(PUSH_UNDEF);
    OPC32(PUSH_I32, 0);
    uint32_t loop = embi_jsc_here(p);
    uint32_t ds = embi_jsc_emit_op2(p, EMBI_JSOP_ASYNC_DELEGATE_CALL, 0, 0);
    OPC(AWAIT);
    uint32_t ck = embi_jsc_emit_op2(p, EMBI_JSOP_ASYNC_DELEGATE_CHECK, 0, 0);
    OPC(ASYNC_YIELD);
    uint32_t rp = jump_emit(p, EMBI_JSOP_ASYNC_RET_PREP);
    OPC(AWAIT);
    OPC(ASYNC_RET_FINISH);
    OPC32(JMP, loop);
    jump_to(p, rp, loop);
    /* No throw method: [rec] is closed, then the TypeError is thrown. */
    jump_to(p, ds + 1u, embi_jsc_here(p));
    p->fn->depth = entry;
    uint32_t cl = jump_emit(p, EMBI_JSOP_ASYNC_CLOSE_CALL);
    OPC(AWAIT);
    OPC32(GEN_RESUME, 0);
    OPC(ASYNC_CLOSE_CHECK);
    jump_to(p, cl, embi_jsc_here(p));
    OPC(ASYNC_DELEGATE_NOTHROW);
    /* No return method: the received value is awaited, then returned. */
    jump_to(p, ds + 5u, embi_jsc_here(p));
    p->fn->depth = entry;
    OPC(AWAIT);
    OPC32(GEN_RESUME, 0);
    jump_to(p, ck + 1u, embi_jsc_here(p));
    p->fn->depth = entry;
    emit_return_top(p);
    jump_to(p, ck + 5u, embi_jsc_here(p));
    p->fn->depth = entry;
}

static bool parse_yield(cunit *p, bool no_in, expr *e)
{
    if (p->fn->in_params) {
        return perr(p, "Yield expression not allowed in formal parameter");
    }
    /* In an async generator (J3b) the operand is awaited before the yield (ASYNC_YIELD takes the
     * awaited value), and a return resumption awaits its value before the return runs. */
    bool ag = p->fn->is_async;
    adv(p); /* yield */
    bool star = false;
    if (is(p, EMBI_JST_MUL) && !(p->tok.flags & EMBI_JST_F_NL_BEFORE)) {
        star = true;
        adv(p);
    }
    if (!star) {
        bool operand = !(p->tok.flags & EMBI_JST_F_NL_BEFORE) && !is(p, EMBI_JST_RPAREN) &&
                       !is(p, EMBI_JST_RBRACK) && !is(p, EMBI_JST_RBRACE) &&
                       !is(p, EMBI_JST_COMMA) && !is(p, EMBI_JST_SEMI) &&
                       !is(p, EMBI_JST_COLON) && !is(p, EMBI_JST_EOF);
        if (operand) {
            expr v;
            if (!parse_assign(p, no_in, &v)) {
                return false;
            }
            to_value(p, &v);
        } else {
            OPC(PUSH_UNDEF);
        }
        if (ag) {
            OPC(AWAIT);
            OPC32(GEN_RESUME, 0);
            OPC(ASYNC_YIELD);
        } else {
            OPC(YIELD);
        }
        uint32_t gr = jump_emit(p, EMBI_JSOP_GEN_RESUME);
        uint32_t j_end = jump_emit(p, EMBI_JSOP_JMP);
        jump_to(p, gr, embi_jsc_here(p));
        if (ag) {
            OPC(AWAIT);
            OPC32(GEN_RESUME, 0);
        }
        emit_return_top(p);
        jump_to(p, j_end, embi_jsc_here(p));
    } else {
        expr v;
        if (!parse_assign(p, no_in, &v)) {
            return false;
        }
        to_value(p, &v);
        if (ag) {
            async_delegate(p);
            set_value(e);
            return true;
        }
        int32_t entry = p->fn->depth;
        OPC(ITER_GET);
        OPC(PUSH_UNDEF);
        OPC32(PUSH_I32, 0);
        uint32_t loop = embi_jsc_here(p);
        uint32_t ds = embi_jsc_emit_op2(p, EMBI_JSOP_DELEGATE_STEP, 0, 0);
        OPC(YIELD_RAW);
        OPC32(JMP, loop);
        jump_to(p, ds + 5u, embi_jsc_here(p));
        p->fn->depth = entry;
        emit_return_top(p);
        jump_to(p, ds + 1u, embi_jsc_here(p));
    }
    set_value(e);
    return true;
}

/* Whether a direct eval call may appear in the right-hand side of the assignment that starts at the
 * current token. Such an eval can declare a var of the target's name, so the reference has to be
 * resolved before the right-hand side runs (the ref ops). Sources without the text "eval" after
 * this point are answered at once; otherwise the tokens up to the first top-level terminator are
 * scanned for an identifier eval. */
static bool eval_in_rhs(cunit *p)
{
    const uint8_t *src = p->src;
    if (!p->eval_last_set) {
        p->eval_last_set = true;
        p->eval_last = UINT32_MAX;
        for (uint32_t i = p->src_len; i >= 4u; i--) {
            if (src[i - 4u] == 'e' && memcmp(src + i - 4u, "eval", 4) == 0) {
                p->eval_last = i - 4u;
                break;
            }
        }
    }
    if (p->eval_last == UINT32_MAX || p->eval_last < p->tok.start) {
        return false;
    }
    pscan s;
    pscan_init(&s, p);
    uint32_t base = s.sp;
    for (;;) {
        if (s.t.kind == EMBI_JST_EOF || s.t.kind == EMBI_JST_ERROR || s.t.start > p->eval_last) {
            return false;
        }
        if (s.t.kind == EMBI_JST_NAME && s.t.end - s.t.start == 4u &&
            memcmp(src + s.t.start, "eval", 4) == 0) {
            return true;
        }
        switch (s.t.kind) {
        case EMBI_JST_LPAREN:
        case EMBI_JST_LBRACK:
        case EMBI_JST_LBRACE:
            if (!scan_open(&s, open_char(s.t.kind))) {
                return true;
            }
            break;
        case EMBI_JST_TPL_HEAD:
            if (!scan_open(&s, 'T')) {
                return true;
            }
            break;
        case EMBI_JST_RPAREN:
        case EMBI_JST_RBRACK:
        case EMBI_JST_RBRACE:
            if (s.sp == base || !scan_closes(s.t.kind, pscan_top(&s))) {
                return false;
            }
            s.sp--;
            break;
        case EMBI_JST_COMMA:
        case EMBI_JST_SEMI:
            if (s.sp == base) {
                return false;
            }
            break;
        default:
            break;
        }
        pscan_next(&s);
    }
}


/* Whether reading the name in the current scope cannot run code: it names a binding of a function
 * scope (not a property of the global object, whose accessor would run) and no with object lies
 * between the read and that binding. A name that is not declared (yet) is not inert. */
static bool name_inert(const cunit *p, embi_js_value name)
{
    for (const cfunc *g = p->fn; g; g = g->parent) {
        if (g->dynref) {
            return false;
        }
        for (const cscope *sc = g == p->fn ? g->cur : g->def; sc; sc = sc->parent) {
            if (sc->kind == SC_WITH) {
                return false;
            }
            const cbind *b = embi_jsc_find_in_scope((cscope *)sc, name);
            if (b) {
                return !b->global;
            }
        }
        if (g->self_name == name) {
            return true;
        }
    }
    return false;
}

/* Whether the right-hand side that starts at the current token can be evaluated without running
 * code that is visible to the program. Such a right-hand side cannot create the global that a
 * strict assignment to an undeclared name would store to, so that assignment needs no early
 * reference (the common path: a plain SET_GLOBAL). A right-hand side that is not plain makes the
 * reference first and carries whether it was unresolvable to the store (DYN_REF / DYN_SET_REF,
 * ECMA-262 13.15.2 and PutValue step 5.a).
 * Plain is a short run of:
 *   - primitive literals, this, and names that name_inert accepts (a global name may be an
 *     accessor of the global object; an undeclared one may become one);
 *   - array and object literals of those (no computed key, spread, method or accessor);
 *   - operators. The operators that convert their operands (arithmetic, relational, loose
 *     equality, bitwise) can run valueOf / toString / Symbol.toPrimitive, so they are plain only
 *     when every operand of the run is a primitive literal. ===, !==, !, &&, ||, ?? and ?: never
 *     run code.
 * No call, new, member access, assignment, update, delete, function, class, template, regular
 * expression, yield or await. A token run that is not clearly plain is not: the answer errs on
 * the side of an early reference. */
static bool rhs_plain(cunit *p)
{
    embi_jslex lx = p->lx;
    lx.scratch = p->peek_scratch;
    lx.scratch_cap = p->peek_cap;
    embi_jstok t = p->tok;
    uint32_t depth = 0;
    bool operand = false;
    bool converts = false; /* an operator that converts its operands was seen */
    bool nonprim = false;  /* an operand that may be an object was seen */
    uint64_t braces = 0;   /* bit d: the bracket open at depth d + 1 is an object literal */
    uint8_t prev = EMBI_JST_EOF;
    for (uint32_t n = 0; n < 48; n++) {
        bool nl = n > 0 && (t.flags & EMBI_JST_F_NL_BEFORE);
        bool starts = t.kind == EMBI_JST_NAME || t.kind == EMBI_JST_NUMBER || t.kind == EMBI_JST_STRING;
        if (nl && depth == 0 && operand && starts) {
            return !(converts && nonprim); /* automatic semicolon insertion ends the right-hand side */
        }
        switch (t.kind) {
        case EMBI_JST_NAME:
            if (t.flags & EMBI_JST_F_ESCAPED) {
                return false;
            }
            if (t.kw == EMBI_JSKW_typeof || t.kw == EMBI_JSKW_void) {
                operand = false;
            } else if (t.kw == EMBI_JSKW_null || t.kw == EMBI_JSKW_true || t.kw == EMBI_JSKW_false) {
                operand = true;
            } else if (t.kw == EMBI_JSKW_this) {
                operand = true;
                nonprim = true;
            } else if (t.kw == EMBI_JSKW_NONE) {
                if (!name_inert(p, tok_name(p, &t))) {
                    return false;
                }
                operand = true;
                nonprim = true;
            } else {
                return false;
            }
            break;
        case EMBI_JST_NUMBER:
        case EMBI_JST_STRING:
        case EMBI_JST_BIGINT:
            operand = true;
            break;
        case EMBI_JST_LBRACE:
            braces |= 1ull << depth;
            depth++;
            operand = false;
            nonprim = true;
            break;
        case EMBI_JST_LBRACK:
            if (operand) {
                return false; /* a computed member access */
            }
            if (depth > 0 && (braces >> (depth - 1) & 1u) &&
                (prev == EMBI_JST_LBRACE || prev == EMBI_JST_COMMA)) {
                return false; /* a computed key converts it */
            }
            braces &= ~(1ull << depth);
            depth++;
            operand = false;
            nonprim = true;
            break;
        case EMBI_JST_RBRACK:
        case EMBI_JST_RBRACE:
            if (depth == 0) {
                return !(converts && nonprim);
            }
            depth--;
            operand = true;
            break;
        case EMBI_JST_RPAREN:
        case EMBI_JST_SEMI:
        case EMBI_JST_EOF:
            return depth == 0 && !(converts && nonprim);
        case EMBI_JST_COMMA:
            if (depth == 0) {
                return !(converts && nonprim);
            }
            operand = false;
            break;
        case EMBI_JST_ADD:
        case EMBI_JST_SUB:
        case EMBI_JST_MUL:
        case EMBI_JST_MOD:
        case EMBI_JST_LT:
        case EMBI_JST_GT:
        case EMBI_JST_LE:
        case EMBI_JST_GE:
        case EMBI_JST_EQ:
        case EMBI_JST_NE:
        case EMBI_JST_SHL:
        case EMBI_JST_SAR:
        case EMBI_JST_SHR:
        case EMBI_JST_BAND:
        case EMBI_JST_BOR:
        case EMBI_JST_BXOR:
        case EMBI_JST_BNOT:
            converts = true;
            operand = false;
            break;
        case EMBI_JST_SEQ:
        case EMBI_JST_SNE:
        case EMBI_JST_NOT:
        case EMBI_JST_LAND:
        case EMBI_JST_LOR:
        case EMBI_JST_NULLISH:
        case EMBI_JST_QUESTION:
        case EMBI_JST_COLON:
            operand = false;
            break;
        default:
            return false;
        }
        prev = t.kind;
        lx.regex_ok = !operand;
        lex_scan(p, &lx, &t, true);
    }
    return false;
}

static bool parse_assign(cunit *p, bool no_in, expr *e)
{
    NEST_GUARD(p);
    if (p->fn->is_gen && is_kw(p, EMBI_JSKW_yield)) {
        return parse_yield(p, no_in, e);
    }
    if (is_kw(p, EMBI_JSKW_async) && !(p->tok.flags & EMBI_JST_F_ESCAPED) && async_arrow_ahead(p)) {
        uint32_t astart = p->tok.start;
        adv(p); /* async */
        return parse_arrow_async(p, no_in, e, astart);
    }
    if (is(p, EMBI_JST_LPAREN) && arrow_ahead(p)) {
        return parse_arrow(p, no_in, e);
    }
    if (is_ident(p)) {
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind == EMBI_JST_ARROW && !(nt.flags & EMBI_JST_F_NL_BEFORE)) {
            return parse_arrow(p, no_in, e);
        }
    }
    if ((is(p, EMBI_JST_LBRACK) || is(p, EMBI_JST_LBRACE)) && group_follow(p) == GF_ASSIGN) {
        return parse_assign_pattern(p, no_in, e);
    }
    expr lhs;
    if (!parse_cond(p, no_in, &lhs)) {
        return false;
    }
    if (is(p, EMBI_JST_ARROW)) {
        return perr(p, "Malformed arrow function parameter list");
    }
    if (is(p, EMBI_JST_LAND_EQ) || is(p, EMBI_JST_LOR_EQ) || is(p, EMBI_JST_NULLISH_EQ)) {
        if (!assignable(&lhs)) {
            return perr(p, "Invalid left-hand side in assignment");
        }
        if (!check_target(p, &lhs)) {
            return false;
        }
        uint8_t kind = p->tok.kind;
        adv(p);
        if (!logical_assign(p, &lhs, kind, no_in)) {
            return false;
        }
        set_value(e);
        return true;
    }
    int op = assign_op(p->tok.kind);
    if (op == -1) {
        *e = lhs;
        return true;
    }
    if (!assignable(&lhs)) {
        return perr(p, "Invalid left-hand side in assignment");
    }
    if (!check_target(p, &lhs)) {
        return false;
    }
    adv(p);
    bool early = early_ref(p, &lhs) ||
                 (lhs.kind == EX_VAR && !p->fn->strict && !embi_jsc_is_top(p->fn) && eval_in_rhs(p)) ||
                 (lhs.kind == EX_VAR && op == ASSIGN_PLAIN && p->fn->strict &&
                  !name_visible(p, lhs.scope, lhs.name) && !rhs_plain(p));
    if (early) {
        embi_jsc_emit_sym(p, SYM_KIND_REF, lhs.name, lhs.scope, nullptr);
    }
    if (op == ASSIGN_PLAIN) {
        if (lhs.kind == EX_VAR && !lhs.paren) {
            p->hint = lhs.name;
        }
        expr rhs;
        if (!parse_assign(p, no_in, &rhs)) {
            return false;
        }
        to_value(p, &rhs);
    } else {
        if (early) {
            embi_jsc_emit_sym(p, SYM_KIND_GETREF, lhs.name, lhs.scope, nullptr);
        } else {
            ref_dup(p, &lhs);
            ref_get(p, &lhs);
        }
        expr rhs;
        if (!parse_assign(p, no_in, &rhs)) {
            return false;
        }
        to_value(p, &rhs);
        embi_jsc_emit_op(p, (uint8_t)op);
    }
    if (early) {
        embi_jsc_emit_sym(p, SYM_KIND_SETREF, lhs.name, lhs.scope, nullptr);
    } else {
        ref_set(p, &lhs);
    }
    set_value(e);
    return true;
}


static bool parse_expression(cunit *p, bool no_in, expr *e)
{
    if (!parse_assign(p, no_in, e)) {
        return false;
    }
    bool comma = false;
    while (is(p, EMBI_JST_COMMA)) {
        to_value(p, e);
        OPC(POP);
        adv(p);
        expr r;
        if (!parse_assign(p, no_in, &r)) {
            return false;
        }
        *e = r;
        comma = true;
    }
    /* The value of a comma expression is never a reference: (a, b) = 1 is invalid, and
     * (a, b.c)() calls without a receiver. It is not an anonymous function definition either. */
    if (comma) {
        to_value(p, e);
        e->anon = false;
    }
    return true;
}

/* ------------------------------------------------------------------ functions */

/* Whether the token after a directive candidate string ends the statement. Without a newline
 * only ; } EOF do. After a newline the string still continues into an expression statement when
 * the next token can continue an expression ("use strict"
+1). */
static bool directive_ends(const embi_jstok *nt)
{
    switch (nt->kind) {
    case EMBI_JST_SEMI:
    case EMBI_JST_RBRACE:
    case EMBI_JST_EOF:
        return true;
    default:
        break;
    }
    if (!(nt->flags & EMBI_JST_F_NL_BEFORE)) {
        return false;
    }
    switch (nt->kind) {
    case EMBI_JST_NAME:
        return nt->kw != EMBI_JSKW_in && nt->kw != EMBI_JSKW_instanceof;
    case EMBI_JST_NUMBER:
    case EMBI_JST_BIGINT:
    case EMBI_JST_STRING:
    case EMBI_JST_REGEX:
    case EMBI_JST_LBRACE:
    case EMBI_JST_INC:
    case EMBI_JST_DEC:
    case EMBI_JST_NOT:
    case EMBI_JST_BNOT:
        return true;
    default:
        return false;
    }
}

/* One parameter with a default: when its argument is undefined, the default runs. The value
 * (argument or default) is on the stack and initializes the parameter binding. */
static bool parse_param_default(cunit *p, embi_js_value nm)
{
    adv(p); /* = */
    p->hint = nm;
    OPC(DUP);
    OPC(PUSH_UNDEF);
    OPC(SEQ);
    uint32_t skip = jump_emit(p, EMBI_JSOP_JMP_FALSE);
    OPC(POP);
    expr e;
    if (!parse_assign(p, false, &e)) {
        return false;
    }
    to_value(p, &e);
    jump_to(p, skip, embi_jsc_here(p));
    return true;
}

/* Initializes the binding of a parameter from the value on the stack. */
static void param_init(cunit *p, cfunc *f, embi_js_value nm)
{
    cbind *b = embi_jsc_find_in_scope(f->root, nm);
    b->init_pc = embi_jsc_here(p);
    embi_jsc_emit_sym(p, SYM_KIND_INIT, nm, f->root, b);
}

static bool parse_params(cunit *p, cfunc *f)
{
    if (!expect(p, EMBI_JST_LPAREN)) {
        return false;
    }
    /* A rest or default parameter, or a pattern, makes the list non-simple (no mapped arguments). */
    f->simple = true;
    uint32_t n = 0;
    uint32_t len = 0;
    bool len_open = true; /* no default or rest parameter has been seen yet */
    /* Each parameter is initialized by code (the raw argument, or its default, into the parameter
     * binding, in order), so a default can see the earlier parameters and a later one is in its
     * TDZ. A list without a default needs none of it: the code is dropped again below. */
    uint32_t code_mark = (uint32_t)f->code.len;
    uint32_t syms_mark = (uint32_t)f->syms.len;
    bool exprs = false;
    f->in_params = true;
    if (!is(p, EMBI_JST_RPAREN)) {
        for (;;) {
            bool rest = is(p, EMBI_JST_ELLIPSIS);
            if (rest) {
                adv(p);
            }
            if (is(p, EMBI_JST_LBRACE) || is(p, EMBI_JST_LBRACK)) {
                if (rest && f->is_setter) {
                    return perr(p, "Setter function argument must not be a rest parameter");
                }
                /* A pattern parameter: its names are parameters, and the list is not simple. */
                f->simple = false;
                exprs = true;
                bool dflt = !rest && group_follow(p) == GF_ASSIGN;
                if (!parse_param_pattern(p, f, n, rest)) {
                    return false;
                }
                if (rest) {
                    f->has_rest = true;
                    f->rest_slot = n;
                    n++;
                    len_open = false;
                    if (is(p, EMBI_JST_ASSIGN)) {
                        return perr(p, "Rest parameter may not have a default initializer");
                    }
                    if (!is(p, EMBI_JST_RPAREN)) {
                        return perr(p, "Rest parameter must be last formal parameter");
                    }
                    break;
                }
                n++;
                if (dflt) {
                    len_open = false;
                } else if (len_open) {
                    len++;
                }
                if (is(p, EMBI_JST_COMMA)) {
                    adv(p);
                    if (is(p, EMBI_JST_RPAREN)) {
                        break;
                    }
                    continue;
                }
                break;
            }
            if (!is_ident(p)) {
                return unexpected(p);
            }
            embi_js_value nm = name_atom(p);
            adv(p);
            declare_param(p, nm, n);
            OPC32(GET_LOC, n);
            if (rest) {
                if (f->is_setter) {
                    return perr(p, "Setter function argument must not be a rest parameter");
                }
                f->simple = false;
                f->has_rest = true;
                f->rest_slot = n;
                n++;
                len_open = false;
                param_init(p, f, nm);
                if (!is(p, EMBI_JST_RPAREN)) {
                    return perr(p, "Rest parameter must be last formal parameter");
                }
                break;
            }
            n++;
            if (is(p, EMBI_JST_ASSIGN)) {
                f->simple = false;
                len_open = false;
                exprs = true;
                if (!parse_param_default(p, nm)) {
                    return false;
                }
            } else if (len_open) {
                len++;
            }
            param_init(p, f, nm);
            if (is(p, EMBI_JST_COMMA)) {
                adv(p);
                if (is(p, EMBI_JST_RPAREN)) {
                    break;
                }
                continue;
            }
            break;
        }
    }
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    f->length = len;
    f->nparams = n;
    f->in_params = false;
    if (f->is_arrow) {
        /* An eval in an arrow's parameter default may not declare arguments only when the arrow
         * has a parameter of that name (otherwise arguments is the enclosing function's). */
        bool has_args_param = false;
        for (uint32_t i = 0; i < f->pnames.len; i++) {
            has_args_param = has_args_param || f->pnames.data[i] == p->atom_arguments;
        }
        for (uint32_t i = 0; i < f->evals.len; i++) {
            f->evals.data[i].args_bad = f->evals.data[i].args_bad && has_args_param;
        }
    }
    if (!exprs) {
        f->code.len = code_mark;
        f->syms.len = syms_mark;
        f->depth = 0;
    } else {
        /* The parameters are bound in a scope of their own, in TDZ until initialized. The body's
         * vars and functions go to a second scope, so closures in the defaults cannot see them. */
        f->ptdz = true;
        for (uint32_t i = 0; i < f->pnames.len; i++) {
            cbind *pb = embi_jsc_find_in_scope(f->root, f->pnames.data[i]);
            if (pb && pb->kind == BK_PARAM) {
                pb->ptdz = true;
            }
        }
        f->vroot = embi_jsc_new_scope(p, f->root);
        f->cur = f->vroot;
        f->pcopy_at = jump_emit(p, EMBI_JSOP_JMP);
        jump_to(p, f->pcopy_at, embi_jsc_here(p));
    }
    /* Duplicate names are allowed only in a simple parameter list of a non-method function in
     * sloppy code (strict code is checked with the body). */
    if (!f->simple || f->is_method) {
        for (uint32_t i = 0; i < f->pnames.len; i++) {
            for (uint32_t j = 0; j < i; j++) {
                if (f->pnames.data[j] == f->pnames.data[i]) {
                    return perr(p, "Duplicate parameter name not allowed in this context");
                }
            }
        }
    }
    return true;
}


static bool parse_body(cunit *p, cfunc *f)
{
    if (!expect(p, EMBI_JST_LBRACE)) {
        return false;
    }
    if (f->strict && !check_strict_params(p, f)) {
        return false;
    }
    /* Directive prologue and body. */
    bool directives = true;
    bool octal_directive = false;
    while (!is(p, EMBI_JST_RBRACE)) {
        if (is(p, EMBI_JST_EOF)) {
            return unexpected(p);
        }
        if (directives) {
            if (is(p, EMBI_JST_STRING)) {
                embi_jstok nt;
                peek(p, &nt);
                bool ends = directive_ends(&nt);
                if (!ends) {
                    /* An expression statement such as "a" + b ends the prologue. */
                    directives = false;
                } else {
                    if (p->tok.flags & EMBI_JST_F_LEGACY) {
                        octal_directive = true;
                    }
                    if (is_use_strict_directive_here(p)) {
                        if (octal_directive) {
                            return perr(p, "Octal escape sequences are not allowed in strict mode");
                        }
                        if (!f->simple) {
                            return perr(p, "Illegal 'use strict' directive in function with non-simple parameter list");
                        }
                        if (!f->strict) {
                            f->strict = true;
                            if (!check_strict_params(p, f)) {
                                return false;
                            }
                        }
                    }
                }
            } else {
                directives = false;
            }
        }
        if (!parse_statement(p)) {
            return false;
        }
    }
    adv(p); /* } */
    emit_implicit_return(p);
    if (f->epi) {
        embi_jsc_patch_list(p, f->epi, embi_jsc_here(p));
        f->epi = nullptr;
        f->depth = 1;
        emit_fn_return(p);
    }
    return true;
}

static bool parse_function_rest(cunit *p, cfunc *f)
{
    if (!parse_params(p, f)) {
        return false;
    }
    /* A generator's body starts after its parameters are bound (GEN_START creates the object). */
    if (f->is_async) {
        if (f->is_gen) {
            OPC(ASYNC_GEN_START);
        } else {
            OPC(ASYNC_START);
        }
    } else if (f->is_gen) {
        OPC(GEN_START);
    }
    return parse_body(p, f);
}


static bool parse_function_common(cunit *p, embi_js_value name, embi_js_value self_name,
                                  uint32_t src_start, cfunc **out)
{
    NEST_GUARD(p);
    bool gen = p->next_gen;
    p->next_gen = false;
    bool async = p->next_async;
    p->next_async = false;
    cfunc *saved = p->fn;
    ctl *saved_ctl = p->ctl;
    uint32_t saved_block = p->block_depth;
    cblk *saved_blk = p->blk;
    cfunc *f = embi_jsc_new_func(p, saved, saved->cur);
    f->name = name;
    f->self_name = self_name;
    f->is_method = p->next_kind != 0;
    f->is_setter = p->next_kind == 3;
    f->is_gen = gen;
    f->is_async = async;
    p->next_kind = 0;
    p->fn = f;
    p->ctl = nullptr;
    p->block_depth = 0;
    p->blk = nullptr;
    OPC32(JMP, 0);
    bool ok = parse_function_rest(p, f);
    p->fn = saved;
    p->ctl = saved_ctl;
    p->block_depth = saved_block;
    p->blk = saved_blk;
    if (!ok) {
        return false;
    }
    f->src_start = src_start;
    f->src_end = p->last_end;
    f->const_idx = embi_jsc_add_const(p, EMBI_JS_EMPTY);
    CF_PUSH(saved, children, f);
    *out = f;
    return true;
}

/* The rest of an arrow function after its parameter list has been recognised: the parameters, =>,
 * and a block body or an expression body (returned). Arrow parameters have no duplicates. */

static bool parse_arrow_rest(cunit *p, cfunc *f, bool no_in)
{
    if (is(p, EMBI_JST_LPAREN)) {
        if (!parse_params(p, f)) {
            return false;
        }
    } else {
        f->in_params = true;
        bool id_ok = is_ident(p);
        f->in_params = false;
        if (!id_ok) {
            return unexpected(p);
        }
        embi_js_value nm = name_atom(p);
        if (!check_strict_name(p, nm)) {
            return false;
        }
        adv(p);
        declare_param(p, nm, 0);
        f->simple = true;
        f->nparams = 1;
        f->length = 1;
    }
    if (!is(p, EMBI_JST_ARROW) || (p->tok.flags & EMBI_JST_F_NL_BEFORE)) {
        return unexpected(p);
    }
    adv(p);
    for (uint32_t i = 0; i < f->pnames.len; i++) {
        for (uint32_t j = 0; j < i; j++) {
            if (f->pnames.data[j] == f->pnames.data[i]) {
                return perr(p, "Duplicate parameter name not allowed in this context");
            }
        }
    }
    if (f->strict && !check_strict_params(p, f)) {
        return false;
    }
    if (f->is_async) {
        OPC(ASYNC_START);
    }
    if (is(p, EMBI_JST_LBRACE)) {
        return parse_body(p, f);
    }
    expr x;
    if (!parse_assign(p, no_in, &x)) {
        return false;
    }
    to_value(p, &x);
    OPC(RETURN);
    return true;
}


static bool parse_arrow_ex(cunit *p, bool no_in, expr *e, bool async, uint32_t src_start)
{
    NEST_GUARD(p);
    embi_js_value name = p->hint;
    p->hint = EMBI_JS_EMPTY;
    cfunc *saved = p->fn;
    ctl *saved_ctl = p->ctl;
    uint32_t saved_block = p->block_depth;
    cblk *saved_blk = p->blk;
    cfunc *f = embi_jsc_new_func(p, saved, saved->cur);
    f->name = name;
    f->is_arrow = true;
    f->is_async = async;
    p->fn = f;
    p->ctl = nullptr;
    p->block_depth = 0;
    p->blk = nullptr;
    OPC32(JMP, 0);
    bool ok = parse_arrow_rest(p, f, no_in);
    p->fn = saved;
    p->ctl = saved_ctl;
    p->block_depth = saved_block;
    p->blk = saved_blk;
    if (!ok) {
        return false;
    }
    f->src_start = src_start;
    f->src_end = p->last_end;
    f->const_idx = embi_jsc_add_const(p, EMBI_JS_EMPTY);
    CF_PUSH(saved, children, f);
    f->new_pc = embi_jsc_here(p);
    OPC32(NEW_CLOSURE, f->const_idx);
    set_value(e);
    e->anon = true;
    return true;
}

static bool parse_arrow(cunit *p, bool no_in, expr *e)
{
    return parse_arrow_ex(p, no_in, e, false, p->tok.start);
}

static bool parse_arrow_async(cunit *p, bool no_in, expr *e, uint32_t start)
{
    return parse_arrow_ex(p, no_in, e, true, start);
}

/* A function declaration. At the top of a function (or of a script or eval) it is instantiated
 * when the function is entered. Inside a block it is block-scoped (see cblk), and in sloppy code
 * it also copies its closure to the function's var of the same name when the declaration is
 * evaluated (Annex B.3.3), unless that name is a parameter. */

/* True when nm is a parameter name of f (the parameters are all known once the body starts). */
static bool is_param_name(const cfunc *f, embi_js_value nm)
{
    for (uint32_t i = 0; i < f->pnames.len; i++) {
        if (f->pnames.data[i] == nm) {
            return true;
        }
    }
    return false;
}

/* A function declaration. At the top of a function (or of a script or eval) it is instantiated
 * when the function is entered. Inside a block it is block-scoped (see cblk), and in sloppy code
 * it also copies its closure to the function's var of the same name when the declaration is
 * evaluated (Annex B.3.3), unless that name is a parameter. */
static bool parse_function_decl(cunit *p)
{
    return parse_function_decl_kw(p, false);
}

static bool parse_function_decl_kw(cunit *p, bool async)
{
    uint32_t fn_start = p->tok.start;
    if (async) {
        adv(p); /* async */
    }
    adv(p); /* function */
    bool gen = false;
    if (is(p, EMBI_JST_MUL)) {
        gen = true;
        adv(p);
    }
    if (!is_ident(p)) {
        return unexpected(p);
    }
    embi_js_value nm = name_atom(p);
    if (!check_strict_name(p, nm)) {
        return false;
    }
    p->next_gen = gen;
    p->next_async = async;
    cblk *blk = p->blk;
    if (!blk && !check_var_conflict(p, nm)) {
        return false;
    }
    if (!blk && p->eval_params && p->fn->is_eval && !p->fn->strict && nm == p->atom_arguments) {
        return perr(p, "Identifier 'arguments' cannot be declared by eval in a parameter default");
    }
    adv(p);
    if (blk) {
        EMBI_ASSERT(blk->hoist_at != 0);
        cbind *bb = embi_jsc_find_in_scope(blk->scope, nm);
        /* In a block a function is lexical: it may repeat only another plain function declaration
         * (not a generator or async one), and only in sloppy code (Annex B.3.2.4, B.3.3.4); it may
         * not repeat a var of the block or a catch parameter. */
        bool plain = !gen && !async;
        if ((bb && (p->fn->strict || bb->kind != BK_FUNC || !bb->plain_fn || !plain)) ||
            scope_has_var(blk->scope, nm) ||
            (blk->scope->parent && blk->scope->parent->kind == SC_CATCH &&
             embi_jsc_find_in_scope(blk->scope->parent, nm))) {
            return perr(p, "Identifier has already been declared");
        }
        if (!bb) {
            bb = embi_jsc_new_bind(p, p->fn, blk->scope, nm, BK_FUNC);
            bb->plain_fn = plain;
        }
        cfunc *f;
        if (!parse_function_common(p, nm, EMBI_JS_EMPTY, fn_start, &f)) {
            return false;
        }
        cblkfn d = {f, bb, nm};
        embi_vec_push(&blk->fns, d);
        if (plain && blk->annexb && !is_param_name(p->fn, nm) &&
            annexb_var_ok(p, blk->scope->parent, nm)) {
            cfunc *top_fn = p->fn;
            cbind *vb = declare_var(p, nm, BK_VAR);
            embi_jsc_emit_sym(p, SYM_KIND_GET, nm, blk->scope, bb);
            embi_jsc_emit_sym(p, SYM_KIND_SET, nm, top_fn->root, vb);
            OPC(POP);
        }
        return true;
    }
    cbind *b = declare_var(p, nm, BK_FUNC);
    cfunc *f;
    if (!parse_function_common(p, nm, EMBI_JS_EMPTY, fn_start, &f)) {
        return false;
    }
    if (f->strict && is_eval_or_args(p, nm)) {
        return perr(p, "Unexpected eval or arguments in strict mode");
    }
    cdecl d = {f, b, nm};
    CF_PUSH(p->fn, decls, d);
    return true;
}

/* ------------------------------------------------------------------ statements */

static bool consume_semi(cunit *p)
{
    if (is(p, EMBI_JST_SEMI)) {
        adv(p);
        return true;
    }
    if (is(p, EMBI_JST_RBRACE) || is(p, EMBI_JST_EOF) || (p->tok.flags & EMBI_JST_F_NL_BEFORE)) {
        return true;
    }
    return unexpected(p);
}

/* Whether the current `let` token starts a declaration (a binding name, [ or {) rather than an
 * identifier expression. */
static bool let_starts_binding(cunit *p)
{
    embi_jstok lt;
    peek(p, &lt);
    if (lt.kind == EMBI_JST_NAME) {
        /* A reserved word (in, instanceof) after let makes let an identifier expression. */
        return lt.kw == EMBI_JSKW_NONE || !reserved_kw(lt.kw);
    }
    return lt.kind == EMBI_JST_LBRACK || lt.kind == EMBI_JST_LBRACE;
}

/* let and const declarations. Each binding lives in the current scope and is uninitialized (TDZ)
 * until its declaration runs. In a for head, the binding is set to EMPTY before its initializer,
 * since the head runs once per loop entry. */
static bool parse_lexical_decls(cunit *p, bool is_const, bool no_in, bool for_head,
                                embi_js_value *forin, cbind **forin_bind)
{
    uint8_t kind = is_const ? BK_CONST : BK_LET;
    uint32_t ndecl = 0;
    for (;;) {
        if (is(p, EMBI_JST_LBRACE) || is(p, EMBI_JST_LBRACK)) {
            pat c = {true, kind, for_head, p->fn->cur};
            if (!parse_pattern_decl(p, &c, no_in)) {
                return false;
            }
            ndecl++;
            if (is(p, EMBI_JST_COMMA)) {
                adv(p);
                continue;
            }
            return true;
        }
        if (!is_ident(p)) {
            return unexpected(p);
        }
        embi_js_value nm = name_atom(p);
        if (nm == p->atom_let) {
            return perr(p, "let is disallowed as a lexically bound name");
        }
        if (!check_strict_name(p, nm)) {
            return false;
        }
        adv(p);
        ndecl++;
        bool ok;
        cbind *b = declare_lexical(p, nm, kind, &ok);
        if (!ok) {
            return false;
        }
        if (for_head) {
            OPC(PUSH_EMPTY);
            embi_jsc_emit_sym(p, SYM_KIND_INIT, nm, p->fn->cur, b);
        }
        if (is(p, EMBI_JST_ASSIGN)) {
            adv(p);
            p->hint = nm;
            expr e;
            if (!parse_assign(p, no_in, &e)) {
                return false;
            }
            to_value(p, &e);
        } else if (for_head && (is_kw(p, EMBI_JSKW_in) || is_kw(p, EMBI_JSKW_of))) {
            if (ndecl > 1) {
                return perr(p, "Invalid left-hand side in for-in loop: Must have a single binding");
            }
            /* Each iteration initializes the binding (a fresh one per iteration). Every
             * reference of the head runs in the TDZ, so none is treated as initialized. */
            b->init_pc = 0xFFFFFFFFu;
            *forin = nm;
            *forin_bind = b;
            return true;
        } else {
            if (is_const) {
                return perr(p, "Missing initializer in const declaration");
            }
            OPC(PUSH_UNDEF);
        }
        b->init_pc = embi_jsc_here(p);
        embi_jsc_emit_sym(p, SYM_KIND_INIT, nm, p->fn->cur, b);
        if (is(p, EMBI_JST_COMMA)) {
            adv(p);
            continue;
        }
        return true;
    }
}

static bool parse_var_decls(cunit *p, bool no_in, bool for_head, embi_js_value *forin)
{
    uint32_t ndecl = 0;
    for (;;) {
        if (is(p, EMBI_JST_LBRACE) || is(p, EMBI_JST_LBRACK)) {
            pat c = {true, BK_VAR, false, p->fn->cur};
            if (!parse_pattern_decl(p, &c, no_in)) {
                return false;
            }
            ndecl++;
            if (is(p, EMBI_JST_COMMA)) {
                adv(p);
                continue;
            }
            return true;
        }
        if (!is_ident(p)) {
            return unexpected(p);
        }
        embi_js_value nm = name_atom(p);
        if (!check_strict_name(p, nm)) {
            return false;
        }
        adv(p);
        ndecl++;
        if (!check_var_conflict(p, nm)) {
            return false;
        }
        if (p->eval_params && p->fn->is_eval && !p->fn->strict && nm == p->atom_arguments) {
            return perr(p, "Identifier 'arguments' cannot be declared by eval in a parameter default");
        }
        note_var_name(p, nm);
        declare_var(p, nm, BK_VAR);
        if (is(p, EMBI_JST_ASSIGN)) {
            adv(p);
            p->hint = nm;
            /* The binding is resolved before the initializer runs (12.2.2). */
            expr target = {};
            target.kind = EX_VAR;
            target.name = nm;
            target.scope = p->fn->cur;
            bool early = early_ref(p, &target);
            if (early) {
                embi_jsc_emit_sym(p, SYM_KIND_REF, nm, p->fn->cur, nullptr);
            }
            expr e;
            if (!parse_assign(p, no_in, &e)) {
                return false;
            }
            to_value(p, &e);
            embi_jsc_emit_sym(p, early ? SYM_KIND_SETREF : SYM_KIND_SET, nm, p->fn->cur, nullptr);
            OPC(POP);
        } else if (for_head && (is_kw(p, EMBI_JSKW_in) || is_kw(p, EMBI_JSKW_of))) {
            if (ndecl > 1) {
                return perr(p, "Invalid left-hand side in for-in loop: Must have a single binding");
            }
            *forin = nm;
            return true;
        }
        if (is(p, EMBI_JST_COMMA)) {
            adv(p);
            continue;
        }
        return true;
    }
}

/* ------------------------------------------------------------------ blocks */

static void blk_free(cblk *b)
{
    embi_vec_free(&b->fns);
}

/* Whether the block whose first token is the current one may contain the token `function`
 * (a declaration is only a candidate). A scan of the block's tokens from a copy of the lexer.
 * Answers true when unsure: a token that cannot be lexed without knowing the grammar context
 * (a slash), an error, the end of input, or a long block.
 * FP1a: the scan also settles every brace it passes. One that closes before the scan stops holds
 * none of the words; one still open when the scan finds a word holds it. Those answers are kept in
 * source order (p->bm), so the blocks nested in a scanned block are table hits, not new scans. */
static bool block_may_have_fn(cunit *p)
{
    const uint32_t key = p->last_end; /* the end of the '{' that was just consumed */
    while (p->bm_i < p->bm.len && p->bm.data[p->bm_i].key < key) {
        p->bm_i++;
    }
    if (p->bm_i < p->bm.len && p->bm.data[p->bm_i].key == key && p->bm.data[p->bm_i].state != 0) {
        return p->bm.data[p->bm_i].state == 2;
    }
    p->bm.len = 0;
    p->bm_i = 0;
    enum { MAX_OPEN = 64 };
    uint32_t at[MAX_OPEN + 1]; /* at[d]: the memo entry of the brace open at depth d (1 = this block) */
    uint32_t depth = 1;
    {
        cbmemo e = {key, 0};
        embi_vec_push(&p->bm, e);
        at[1] = 0;
    }
    embi_jstok t = p->tok;
    embi_jslex lx = p->lx;
    lx.skim = true;
    lx.scratch = p->peek_scratch;
    lx.scratch_cap = p->peek_cap;
    for (uint32_t n = 0; n < 2048u; n++) {
        switch (t.kind) {
        case EMBI_JST_LBRACE:
            depth++;
            if (depth <= MAX_OPEN) {
                cbmemo e = {t.end, 0};
                embi_vec_push(&p->bm, e);
                at[depth] = p->bm.len - 1u;
            }
            break;
        case EMBI_JST_RBRACE: {
            uint32_t d = depth--;
            if (d <= MAX_OPEN) {
                p->bm.data[at[d]].state = 1;
            }
            if (d == 1) {
                return false;
            }
            break;
        }
        case EMBI_JST_NAME:
            if (t.kw == EMBI_JSKW_function || t.kw == EMBI_JSKW_let || t.kw == EMBI_JSKW_const ||
                t.kw == EMBI_JSKW_class) {
                for (uint32_t d = 1; d <= depth && d <= MAX_OPEN; d++) {
                    p->bm.data[at[d]].state = 2;
                }
                return true;
            }
            break;
        case EMBI_JST_DIV:
        case EMBI_JST_DIV_EQ:
        case EMBI_JST_REGEX:
        case EMBI_JST_TPL_NOSUB:
        case EMBI_JST_TPL_HEAD:
        case EMBI_JST_TPL_MIDDLE:
        case EMBI_JST_TPL_TAIL:
        case EMBI_JST_ERROR:
        case EMBI_JST_EOF:
            return true;
        default:
            break;
        }
        lex_scan(p, &lx, &t, true);
    }
    return true;
}

/* Enters a block: its statements run in a new scope, and the entry jump is emitted that the
 * hoisting stub is patched into (block_leave). */
static void block_enter(cunit *p, cblk *b, bool maybe_fn)
{
    cfunc *f = p->fn;
    b->p = p;
    b->fn = f;
    b->saved_cur = f->cur;
    b->outer = p->blk;
    b->scope = embi_jsc_new_scope(p, f->cur);
    b->annexb = !f->strict;
    /* A block that cannot hold a function declaration needs no entry jump. */
    b->hoist_at = maybe_fn ? jump_emit(p, EMBI_JSOP_JMP) : 0;
    f->cur = b->scope;
    p->blk = b;
}

/* Leaves a block. With function declarations, the code after the body is a hoisting stub: for
 * each closure it creates the binding (a fresh box when the binding is captured, made before the
 * closure is created so that the closure can refer to its own name), stores the closure, and
 * jumps back to the body. The entry jump reaches the stub; normal completion steps over it. */
static void block_leave(cunit *p, cblk *b)
{
    uint32_t resume = b->hoist_at + 4u;
    bool lexical = false;
    for (cbind *x = b->scope ? b->scope->first : nullptr; x; x = x->next_in_scope) {
        lexical = lexical || x->kind == BK_LET || x->kind == BK_CONST;
    }
    if (b->hoist_at == 0) {
        EMBI_ASSERT(b->fns.len == 0 && !lexical);
    } else if (b->fns.len == 0 && !lexical) {
        jump_to(p, b->hoist_at, resume);
    } else {
        uint32_t skip = jump_emit(p, EMBI_JSOP_JMP);
        jump_to(p, b->hoist_at, embi_jsc_here(p));
        /* The lexical bindings of the block start uninitialized, each time the block is entered
         * (a fresh box for a captured one). */
        for (cbind *x = b->scope->first; x; x = x->next_in_scope) {
            if (x->kind == BK_LET || x->kind == BK_CONST) {
                OPC(PUSH_EMPTY);
                x->prebox = true;
                x->stub_pc = embi_jsc_here(p);
                embi_jsc_emit_sym(p, SYM_KIND_INIT, x->name, b->scope, x);
            }
        }
        for (uint32_t i = 0; i < b->fns.len; i++) {
            const cblkfn *d = &b->fns.data[i];
            bool first = true;
            for (uint32_t j = 0; j < i; j++) {
                first = first && b->fns.data[j].bind != d->bind;
            }
            if (first) {
                OPC(PUSH_UNDEF);
                embi_jsc_emit_sym(p, SYM_KIND_INIT, d->name, b->scope, d->bind);
            }
            OPC32(NEW_CLOSURE, d->child->const_idx);
            embi_jsc_emit_sym(p, SYM_KIND_SET, d->name, b->scope, d->bind);
            OPC(POP);
        }
        jump_to(p, jump_emit(p, EMBI_JSOP_JMP), resume);
        jump_to(p, skip, embi_jsc_here(p));
    }
    b->fn->cur = b->saved_cur;
    p->blk = b->outer;
}

static bool parse_block(cunit *p)
{
    if (!expect(p, EMBI_JST_LBRACE)) {
        return false;
    }
    cblk b __attribute__((cleanup(blk_free))) = {};
    block_enter(p, &b, block_may_have_fn(p));
    p->block_depth++;
    while (!is(p, EMBI_JST_RBRACE)) {
        if (is(p, EMBI_JST_EOF)) {
            return unexpected(p);
        }
        if (!parse_statement(p)) {
            return false;
        }
    }
    p->block_depth--;
    adv(p);
    block_leave(p, &b);
    return true;
}

/* A statement in a position where a function declaration is not allowed: the body of a loop or
 * of a with statement. */
/* Whether the statement at the current token is a labelled function declaration (label: function
 * f() {}), which no loop, if or with body may be. */
static bool labelled_fn_ahead(cunit *p)
{
    if (!is_ident(p)) {
        return false;
    }
    embi_jslex lx = p->lx;
    lx.scratch = p->peek_scratch;
    lx.scratch_cap = p->peek_cap;
    for (uint32_t n = 0; n < 16u; n++) {
        embi_jstok t;
        lex_scan(p, &lx, &t, true);
        if (t.kind != EMBI_JST_COLON) {
            return false;
        }
        lex_scan(p, &lx, &t, true);
        if (t.kind != EMBI_JST_NAME) {
            return false;
        }
        if (t.kw == EMBI_JSKW_function && !(t.flags & EMBI_JST_F_ESCAPED)) {
            return true;
        }
    }
    return false;
}

/* True when the current async token starts an async function declaration (no line break). */
static bool async_function_ahead(cunit *p)
{
    embi_jstok nt;
    peek(p, &nt);
    return nt.kind == EMBI_JST_NAME && nt.kw == EMBI_JSKW_function &&
           !(nt.flags & (EMBI_JST_F_NL_BEFORE | EMBI_JST_F_ESCAPED)) &&
           !(p->tok.flags & EMBI_JST_F_ESCAPED);
}

/* A sloppy-mode plain function declaration ahead (Annex B.3.4 allows it as an if body or after a
 * label); a generator declaration (function*) is never allowed there. */
static bool annexb_function_ahead(cunit *p)
{
    if (!is_kw(p, EMBI_JSKW_function) || p->fn->strict) {
        return false;
    }
    embi_jstok nt;
    peek(p, &nt);
    return nt.kind != EMBI_JST_MUL;
}

static bool parse_sub_statement(cunit *p)
{
    if (is_kw(p, EMBI_JSKW_class) && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
        return perr(p, "Class declarations are not allowed in this position");
    }
    if (labelled_fn_ahead(p)) {
        return perr(p, "Labelled function declarations are not allowed in this position");
    }
    if (is_kw(p, EMBI_JSKW_async) && async_function_ahead(p)) {
        return perr(p, "Async function declarations are not allowed in this position");
    }
    if (is_kw(p, EMBI_JSKW_function)) {
        return perr(p, "Function declarations are not allowed in this position");
    }
    if (is_kw(p, EMBI_JSKW_const)) {
        return perr(p, "Lexical declaration cannot appear in a single-statement context");
    }
    if (is_kw(p, EMBI_JSKW_let)) {
        /* let [ never starts an expression statement; let followed by a binding on the same line
         * would be a declaration, which this position does not allow. */
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind == EMBI_JST_LBRACK ||
            (let_starts_binding(p) && !(nt.flags & EMBI_JST_F_NL_BEFORE))) {
            return perr(p, "Lexical declaration cannot appear in a single-statement context");
        }
    }
    return parse_statement_ctx(p, true);
}

/* The body of an if statement (and of its else). Sloppy code allows a function declaration there,
 * which behaves as if it were the only statement of a block (Annex B.3.4). */
static bool parse_if_body(cunit *p)
{
    if (!annexb_function_ahead(p)) {
        return parse_sub_statement(p);
    }
    cblk b __attribute__((cleanup(blk_free))) = {};
    block_enter(p, &b, true);
    if (!parse_function_decl(p)) {
        return false;
    }
    block_leave(p, &b);
    return true;
}

/* The body of a loop, switch or labelled statement: runs with its control entry on the stack. */
static bool parse_loop_body(cunit *p)
{
    p->npending = 0;
    return parse_sub_statement(p);
}

/* An `else if` chain is parsed in a loop, not by recursion, so long generated chains don't use
 * native stack per arm. Each arm's end jump is patched to the end of the whole chain. */
static bool parse_if(cunit *p)
{
    /* FP2a: the end jumps live in the unit's arena (a heap vector per if statement was a malloc and a free each). */
    uint32_t *ends = nullptr;
    uint32_t nends = 0;
    uint32_t ends_cap = 0;
    bool ok = false;
    for (;;) {
        adv(p);
        reset_completion(p);
        if (!expect(p, EMBI_JST_LPAREN)) {
            goto out;
        }
        expr c;
        if (!parse_expression(p, false, &c)) {
            goto out;
        }
        to_value(p, &c);
        if (!expect(p, EMBI_JST_RPAREN)) {
            goto out;
        }
        uint32_t jf = jump_emit(p, EMBI_JSOP_JMP_FALSE);
        if (!parse_if_body(p)) {
            goto out;
        }
        if (!is_kw(p, EMBI_JSKW_else)) {
            jump_to(p, jf, embi_jsc_here(p));
            break;
        }
        adv(p);
        if (nends == ends_cap) {
            ends = embi_jsc_vgrow(&p->arena, ends, &ends_cap, sizeof *ends);
        }
        ends[nends++] = jump_emit(p, EMBI_JSOP_JMP);
        jump_to(p, jf, embi_jsc_here(p));
        if (is_kw(p, EMBI_JSKW_if)) {
            continue; /* else if: the next arm, at this nesting level */
        }
        if (!parse_if_body(p)) {
            goto out;
        }
        break;
    }
    for (uint32_t i = 0; i < nends; i++) {
        jump_to(p, ends[i], embi_jsc_here(p));
    }
    ok = true;
out:
    return ok;
}

static bool parse_while(cunit *p)
{
    adv(p);
    reset_completion(p);
    ctl c = {nullptr, CTL_LOOP, 0, 0, {0}, nullptr, nullptr, nullptr, nullptr, 2, nullptr, nullptr, false, false, nullptr};
    take_labels(p, &c);
    c.parent = p->ctl;
    p->ctl = &c;
    uint32_t top = embi_jsc_here(p);
    if (!expect(p, EMBI_JST_LPAREN)) {
        return false;
    }
    expr cond;
    if (!parse_expression(p, false, &cond)) {
        return false;
    }
    to_value(p, &cond);
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    uint32_t jf = jump_emit(p, EMBI_JSOP_JMP_FALSE);
    if (!parse_loop_body(p)) {
        return false;
    }
    embi_jsc_patch_list(p, c.cont, top);
    OPC32(JMP, top);
    jump_to(p, jf, embi_jsc_here(p));
    embi_jsc_patch_list(p, c.brk, embi_jsc_here(p));
    p->ctl = c.parent;
    return true;
}

static bool parse_do(cunit *p)
{
    adv(p);
    reset_completion(p);
    ctl c = {nullptr, CTL_LOOP, 0, 0, {0}, nullptr, nullptr, nullptr, nullptr, 2, nullptr, nullptr, false, false, nullptr};
    take_labels(p, &c);
    c.parent = p->ctl;
    p->ctl = &c;
    uint32_t top = embi_jsc_here(p);
    if (!parse_loop_body(p)) {
        return false;
    }
    if (!is_kw(p, EMBI_JSKW_while)) {
        return unexpected(p);
    }
    adv(p);
    embi_jsc_patch_list(p, c.cont, embi_jsc_here(p));
    if (!expect(p, EMBI_JST_LPAREN)) {
        return false;
    }
    expr cond;
    if (!parse_expression(p, false, &cond)) {
        return false;
    }
    to_value(p, &cond);
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    if (is(p, EMBI_JST_SEMI)) {
        adv(p);
    }
    /* The loop back-edge is an unconditional backward JMP, which checks the interrupt hook. */
    uint32_t jf = jump_emit(p, EMBI_JSOP_JMP_FALSE);
    OPC32(JMP, top);
    jump_to(p, jf, embi_jsc_here(p));
    embi_jsc_patch_list(p, c.brk, embi_jsc_here(p));
    p->ctl = c.parent;
    return true;
}

/* The target of a for-in: a name, or a member expression whose base (and key) were evaluated
 * once before the loop, into temps. */
typedef struct forin_target {
    embi_js_value name;
    cscope *scope;
    expr_kind member; /* EX_VAR for a name; else EX_FIELD, EX_INDEX, EX_PRIV or a super reference */
    uint32_t key;     /* EX_FIELD: constant index */
    cbind *tb;        /* member base */
    cbind *tk;        /* EX_INDEX: key; EX_SUPER_FIELD and EX_SUPER_INDEX: the home object */
    cbind *tx;        /* EX_SUPER_INDEX: key */
    bool replay;      /* a member target: its code runs once per iteration */
    uint32_t tpc;     /* replay: the target code's first pc */
    uint32_t back_at; /* replay: operand of the jump that ends the target code (to the SET) */
    cbind *direct;    /* a let or const head: the binding, initialized on every iteration */
    cscope *restore;  /* a let or const head: the scope to return to after the loop */
    bool pattern;     /* a destructuring target: the replayed code consumes the key */
} forin_target;

/* The target of a destructuring for-in head. Its code sits before the loop and is replayed with
 * the key on the stack (see parse_for_in_tail). The names of a binding pattern are declared
 * already (pat_declare_names). */
static bool forin_pattern(cunit *p, const pat *c, forin_target *ft)
{
    cfunc *f = p->fn;
    uint32_t init_jump = jump_emit(p, EMBI_JSOP_JMP);
    jump_to(p, init_jump, embi_jsc_here(p));
    uint32_t target_pc = embi_jsc_here(p);
    int32_t saved = f->depth;
    f->depth = saved + 2; /* the iterator and the key */
    if (!pat_consume(p, c)) {
        return false;
    }
    f->depth = saved;
    ft->pattern = true;
    ft->replay = true;
    ft->tpc = target_pc;
    ft->back_at = jump_emit(p, EMBI_JSOP_JMP);
    jump_to(p, init_jump, embi_jsc_here(p));
    return true;
}

/* What the head at the current token starts with, when it is a bracket group (a pattern). */
static int head_follow(cunit *p)
{
    if (!is(p, EMBI_JST_LBRACK) && !is(p, EMBI_JST_LBRACE)) {
        return GF_OTHER;
    }
    return group_follow(p);
}

/* The tail of a for-in or for-of head: the object, then the loop. A for-of keeps its iterator
 * record on the stack (ITER_INIT, ITER_NEXT). Its body, from the value fetch to the end of the
 * binding and the body, is protected: a throw there closes the iterator and rethrows (the record
 * is then the value below the exception). Leaving the loop by break or return closes it through
 * pop_items. */
static bool parse_for_in_tail(cunit *p, ctl *c, const forin_target *t)
{
    bool of = is_kw(p, EMBI_JSKW_of);
    if (c->is_async_iter && !of) {
        return perr(p, "for await requires an of loop");
    }
    adv(p); /* in, or of */
    expr obj;
    if (of) {
        if (!parse_assign(p, false, &obj)) {
            return false;
        }
    } else if (!parse_expression(p, false, &obj)) {
        return false;
    }
    to_value(p, &obj);
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    if (of) {
        if (c->is_async_iter) {
            OPC(ASYNC_ITER_INIT);
        } else {
            OPC(ITER_INIT);
        }
        c->is_iter = true;
    } else {
        OPC(FOR_IN_INIT);
    }
    c->items = 1;
    uint32_t next_pc = embi_jsc_here(p);
    int32_t iter_depth = p->fn->depth;
    uint32_t fin_at;
    if (c->is_async_iter) {
        /* [rec] -> [rec v]: the next call, its awaited result, and the done test. */
        OPC(ASYNC_ITER_CALL);
        OPC(AWAIT);
        OPC32(GEN_RESUME, 0);
        fin_at = jump_emit(p, EMBI_JSOP_ASYNC_ITER_STEP);
    } else {
        fin_at = jump_emit(p, of ? EMBI_JSOP_ITER_NEXT : EMBI_JSOP_FOR_IN_NEXT);
    }
    uint32_t range_start = embi_jsc_here(p);
    if (t->replay) {
        /* ES5 12.6.4: the target reference is evaluated for each key. The target code sits before
         * the loop; it runs here with the key on the stack and returns to back_at. */
        jump_to(p, jump_emit(p, EMBI_JSOP_JMP), t->tpc);
        jump_to(p, t->back_at, embi_jsc_here(p));
    }
    if (t->pattern) {
        /* The replayed pattern consumed the key: the loop body runs with the iterator only. */
        p->fn->depth = iter_depth;
    } else if (t->member == EX_VAR && t->direct) {
        t->direct->init_pc = embi_jsc_here(p);
        embi_jsc_emit_sym(p, SYM_KIND_INIT, t->name, t->scope, t->direct);
    } else if (t->member == EX_VAR) {
        embi_jsc_emit_sym(p, SYM_KIND_SET, t->name, t->scope, nullptr);
    } else if (t->member == EX_FIELD) {
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tb);
        OPC(SWAP);
        OPC32(SET_FIELD, t->key);
    } else if (t->member == EX_SUPER_FIELD) {
        /* [key] -> [this home key] */
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tb);
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tk);
        OPC(ROT3);
        OPC(ROT3);
        OPC32(SUPER_SET, t->key);
    } else if (t->member == EX_SUPER_INDEX) {
        /* [key] -> [this home kx key] */
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tb);
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tk);
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tx);
        OPC(ROT4);
        OPC(ROT4);
        OPC(ROT4);
        OPC(SUPER_SET_INDEX);
    } else {
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tb);
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, t->tk);
        OPC(ROT3);
        OPC(ROT3);
        if (t->member == EX_PRIV) {
            OPC(PRIV_SET);
        } else {
            OPC(SET_INDEX);
        }
    }
    /* INIT consumes the key; the other targets leave their value for this POP. */
    if (!t->pattern && !(t->member == EX_VAR && t->direct)) {
        OPC(POP);
    }
    if (!parse_loop_body(p)) {
        return false;
    }
    uint32_t range_end = embi_jsc_here(p);
    OPC32(JMP, next_pc);
    uint32_t done = embi_jsc_here(p);
    jump_to(p, fin_at, done);
    OPC(POP);
    if (of) {
        /* A throw in the body closes the iterator: [record exception] -> [exception] -> rethrow. */
        uint32_t j_exit = jump_emit(p, EMBI_JSOP_JMP);
        uint32_t handler = embi_jsc_here(p);
        p->fn->depth = iter_depth + 1;
        OPC(SWAP);
        uint32_t aw = 0;
        uint32_t aw_end = 0;
        if (c->is_async_iter) {
            /* [exc rec]: the return call and its await. A throw there (or a rejected await) is
             * dropped and the original exception rethrown, so the handler at h2 pops it. */
            aw = embi_jsc_here(p);
            uint32_t jl = jump_emit(p, EMBI_JSOP_ASYNC_CLOSE_CALL);
            OPC(AWAIT);
            OPC32(GEN_RESUME, 0);
            aw_end = embi_jsc_here(p);
            OPC(POP);
            uint32_t jskip = jump_emit(p, EMBI_JSOP_JMP);
            uint32_t h2 = embi_jsc_here(p);
            p->fn->depth = iter_depth + 2;
            OPC(POP);
            uint32_t lpop = embi_jsc_here(p);
            jump_to(p, jl, lpop);
            jump_to(p, jskip, lpop);
            OPC(POP);
            embi_js_exc_entry ae = {aw, aw_end, h2, (uint32_t)iter_depth + 1u, 0};
            CF_PUSH(p->fn, exc, ae);
        } else {
            OPC(ITER_CLOSE_THROW);
        }
        OPC(RETHROW);
        embi_js_exc_entry ce = {range_start, range_end, handler, (uint32_t)iter_depth, 0};
        CF_PUSH(p->fn, exc, ce);
        if (t->replay) {
            /* The replayed target code (its base, its key, the assignment or the pattern) also
             * runs with the record below the key, and its throws close the iterator. The range
             * covers the code up to and including the jump that returns to the body. */
            embi_js_exc_entry re = {t->tpc, t->back_at + 4u, handler, (uint32_t)iter_depth, 0};
            CF_PUSH(p->fn, exc, re);
        }
        jump_to(p, j_exit, embi_jsc_here(p));
    }
    embi_jsc_patch_list(p, c->cont, next_pc);
    embi_jsc_patch_list(p, c->brk, embi_jsc_here(p));
    p->ctl = c->parent;
    if (t->restore) {
        p->fn->cur = t->restore;
    }
    return true;
}

/* Copies the let bindings of a for head into fresh bindings (CreatePerIterationEnvironment): the
 * value is read from the old binding and stored into a new one, so closures made by earlier
 * iterations keep theirs. */
static void emit_iter_copy(cunit *p, cscope *head)
{
    for (cbind *b = head->first; b; b = b->next_in_scope) {
        if (b->kind == BK_LET) {
            embi_jsc_emit_sym(p, SYM_KIND_GET, b->name, head, b);
            embi_jsc_emit_sym(p, SYM_KIND_INIT, b->name, head, b);
        }
    }
}

static bool parse_for(cunit *p)
{
    adv(p); /* for */
    reset_completion(p);
    bool await_loop = false;
    if (is_kw(p, EMBI_JSKW_await)) {
        if (!p->fn->is_async) {
            return perr(p, "for await is only valid in async functions");
        }
        await_loop = true;
        if (p->fn == p->top) {
            p->fn->uses_await = true;
        }
        adv(p);
    }
    if (!expect(p, EMBI_JST_LPAREN)) {
        return false;
    }
    ctl c = {nullptr, CTL_LOOP, 0, 0, {0}, nullptr, nullptr, nullptr, nullptr, 2, nullptr, nullptr, false, false, nullptr};
    take_labels(p, &c);
    c.parent = p->ctl;
    c.is_async_iter = await_loop;
    p->ctl = &c;

    embi_js_value forin_name = EMBI_JS_EMPTY;
    cbind *forin_bind = nullptr;
    forin_target ft = {EMBI_JS_EMPTY, nullptr, EX_VAR, 0, nullptr, nullptr, nullptr, false, 0, 0,
                       nullptr, nullptr, false};
    bool have_forin_expr = false;
    /* A let or const head: its bindings live in their own scope, which the loop body sees. */
    cscope *head = nullptr;
    cscope *head_saved = nullptr;
    bool lexical_head = false;
    if (is(p, EMBI_JST_SEMI)) {
        /* no initializer */
    } else if (is_kw(p, EMBI_JSKW_var)) {
        adv(p);
        int hf = head_follow(p);
        if (hf == GF_IN || hf == GF_OF) {
            pat pt = {true, BK_VAR, false, p->fn->cur};
            if (!pat_declare_names(p, &pt) || !forin_pattern(p, &pt, &ft)) {
                return false;
            }
            have_forin_expr = true;
        } else {
            if (!parse_var_decls(p, true, true, &forin_name)) {
                return false;
            }
            if (forin_name == EMBI_JS_EMPTY && is_kw(p, EMBI_JSKW_of)) {
                return perr(p, "for-of loop variable declaration may not have an initializer.");
            }
        }
    } else if (is_kw(p, EMBI_JSKW_const) || (is_kw(p, EMBI_JSKW_let) && let_starts_binding(p))) {
        bool is_const = is_kw(p, EMBI_JSKW_const);
        adv(p);
        head_saved = p->fn->cur;
        head = embi_jsc_new_scope(p, head_saved);
        p->fn->cur = head;
        lexical_head = true;
        int hf = head_follow(p);
        if (hf == GF_IN || hf == GF_OF) {
            pat pt = {true, is_const ? BK_CONST : BK_LET, true, head};
            ft.restore = head_saved;
            if (!pat_declare_names(p, &pt) || !forin_pattern(p, &pt, &ft)) {
                return false;
            }
            have_forin_expr = true;
        } else {
            if (!parse_lexical_decls(p, is_const, true, true, &forin_name, &forin_bind)) {
                return false;
            }
            if (forin_name == EMBI_JS_EMPTY && is_kw(p, EMBI_JSKW_of)) {
                return perr(p, "for-of loop variable declaration may not have an initializer.");
            }
        }
    } else if (head_follow(p) == GF_IN || head_follow(p) == GF_OF) {
        /* A destructuring assignment target of a for-in or for-of head. */
        pat pt = {false, 0, false, nullptr};
        if (!forin_pattern(p, &pt, &ft)) {
            return false;
        }
        have_forin_expr = true;
    } else {
        if (!await_loop && is_kw(p, EMBI_JSKW_async) && !(p->tok.flags & EMBI_JST_F_ESCAPED)) {
            embi_jstok nt;
            peek(p, &nt);
            bool of_arrow = false; /* for (async of => {}; ...) is an ordinary async arrow */
            if (nt.kind == EMBI_JST_NAME && nt.kw == EMBI_JSKW_of && !(nt.flags & EMBI_JST_F_ESCAPED)) {
                embi_jslex lx = p->lx;
                lx.scratch = p->peek_scratch;
                lx.scratch_cap = p->peek_cap;
                embi_jstok t1;
                embi_jstok t2;
                lex_scan(p, &lx, &t1, true);
                lex_scan(p, &lx, &t2, true);
                of_arrow = t2.kind == EMBI_JST_ARROW;
            }
            if (nt.kind == EMBI_JST_NAME && nt.kw == EMBI_JSKW_of && !(nt.flags & EMBI_JST_F_ESCAPED) &&
                !of_arrow) {
                return perr(p, "The left-hand side of a for-of loop may not be 'async'");
            }
        }
        /* The expression's code may be a for-in member target, which is placed before the loop
         * head and replayed from there. A jump skips it at the loop entry (a no-op otherwise). */
        uint32_t init_jump = jump_emit(p, EMBI_JSOP_JMP);
        jump_to(p, init_jump, embi_jsc_here(p));
        uint32_t target_pc = embi_jsc_here(p);
        int32_t saved_max = p->fn->max_depth;
        p->fn->max_depth = p->fn->depth;
        expr e;
        if (!parse_expression(p, true, &e)) {
            return false;
        }
        int32_t target_max = p->fn->max_depth;
        p->fn->max_depth = saved_max > target_max ? saved_max : target_max;
        if (is_kw(p, EMBI_JSKW_in) || is_kw(p, EMBI_JSKW_of)) {
            if (!assignable(&e)) {
                return perr(p, "Invalid left-hand side in for-in");
            }
            if (!check_target(p, &e)) {
                return false;
            }
            ft.member = e.kind;
            if (e.kind == EX_VAR) {
                ft.name = e.name;
                ft.scope = e.scope;
            } else {
                /* The base and key are stored in temporaries by the target code, which runs for
                 * every key (see parse_for_in_tail). */
                cfunc *f = p->fn;
                if (e.kind == EX_SUPER_INDEX) {
                    ft.tx = embi_jsc_new_bind(p, f, nullptr, EMBI_JS_EMPTY, BK_TEMP);
                    embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, ft.tx);
                }
                if (e.kind == EX_INDEX || e.kind == EX_PRIV || e.kind == EX_SUPER_FIELD ||
                    e.kind == EX_SUPER_INDEX) {
                    /* A private name's key is the private symbol on the stack, like an index;
                     * a super reference keeps its home object here. */
                    ft.tk = embi_jsc_new_bind(p, f, nullptr, EMBI_JS_EMPTY, BK_TEMP);
                    embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, ft.tk);
                }
                if (e.kind == EX_FIELD || e.kind == EX_SUPER_FIELD) {
                    ft.key = e.key;
                }
                ft.tb = embi_jsc_new_bind(p, f, nullptr, EMBI_JS_EMPTY, BK_TEMP);
                embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, ft.tb);
                ft.replay = true;
                ft.tpc = target_pc;
                /* The target code runs with the iterator and the key on the stack. */
                if (target_max + 4 > p->fn->max_depth) {
                    p->fn->max_depth = target_max + 4;
                }
                ft.back_at = jump_emit(p, EMBI_JSOP_JMP);
                jump_to(p, init_jump, embi_jsc_here(p));
            }
            have_forin_expr = true;
        } else {
            to_value(p, &e);
            OPC(POP);
        }
    }

    if (have_forin_expr) {
        return parse_for_in_tail(p, &c, &ft);
    }
    if (forin_name != EMBI_JS_EMPTY) {
        ft.name = forin_name;
        ft.scope = p->fn->cur;
        ft.direct = forin_bind;
        ft.restore = head_saved;
        return parse_for_in_tail(p, &c, &ft);
    }

    if (!expect(p, EMBI_JST_SEMI)) {
        return false;
    }
    if (lexical_head) {
        emit_iter_copy(p, head);
    }
    uint32_t test_pc = embi_jsc_here(p);
    uint32_t jf = 0;
    bool has_test = false;
    if (!is(p, EMBI_JST_SEMI)) {
        expr t;
        if (!parse_expression(p, false, &t)) {
            return false;
        }
        to_value(p, &t);
        jf = jump_emit(p, EMBI_JSOP_JMP_FALSE);
        has_test = true;
    }
    if (!expect(p, EMBI_JST_SEMI)) {
        return false;
    }
    uint32_t j_body = jump_emit(p, EMBI_JSOP_JMP);
    uint32_t upd_pc = embi_jsc_here(p);
    if (lexical_head) {
        emit_iter_copy(p, head);
    }
    if (!is(p, EMBI_JST_RPAREN)) {
        expr u;
        if (!parse_expression(p, false, &u)) {
            return false;
        }
        to_value(p, &u);
        OPC(POP);
    }
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    OPC32(JMP, test_pc);
    jump_to(p, j_body, embi_jsc_here(p));
    if (!parse_loop_body(p)) {
        return false;
    }
    embi_jsc_patch_list(p, c.cont, upd_pc);
    OPC32(JMP, upd_pc);
    if (has_test) {
        jump_to(p, jf, embi_jsc_here(p));
    }
    embi_jsc_patch_list(p, c.brk, embi_jsc_here(p));
    p->ctl = c.parent;
    if (lexical_head) {
        p->fn->cur = head_saved;
    }
    return true;
}

static bool parse_break_continue(cunit *p, bool is_continue)
{
    adv(p);
    embi_js_value label = EMBI_JS_EMPTY;
    if (is_ident(p) && !(p->tok.flags & EMBI_JST_F_NL_BEFORE)) {
        label = name_atom(p);
        adv(p);
    }
    ctl *target = nullptr;
    for (ctl *c = p->ctl; c; c = c->parent) {
        if (c->kind == CTL_TRY_FIN || c->kind == CTL_TRY_CATCH) {
            continue;
        }
        if (label != EMBI_JS_EMPTY) {
            bool hit = false;
            for (uint8_t i = 0; i < c->nlabels; i++) {
                if (c->labels[i] == label) {
                    hit = true;
                }
            }
            if (hit) {
                target = c;
                break;
            }
        } else if (c->kind == CTL_LOOP || (!is_continue && c->kind == CTL_SWITCH)) {
            target = c;
            break;
        }
    }
    if (!target) {
        if (label != EMBI_JS_EMPTY) {
            return perr(p, "Undefined label");
        }
        return perr(p, is_continue ? "Illegal continue statement: no surrounding iteration statement"
                                   : "Illegal break statement");
    }
    if (is_continue && target->kind != CTL_LOOP) {
        return perr(p, "Illegal continue statement");
    }
    int32_t saved = p->fn->depth;
    emit_unwind(p, target, is_continue, false);
    p->fn->depth = saved;
    return consume_semi(p);
}

static bool parse_return(cunit *p)
{
    if (embi_jsc_is_top(p->fn) || p->fn->is_static_block) {
        return perr(p, "Illegal return statement");
    }
    adv(p);
    /* emit_return_top leaves the depth with the value still counted (a yield needs that: its normal
     * path joins with the received value on the stack). A return statement consumes the value, so the
     * depth after it is the depth before it: a later for-of would otherwise place its exception
     * handler's depth one slot too high, and the handler would close a stale slot as the record. */
    int32_t depth0 = p->fn->depth;
    if (!is(p, EMBI_JST_SEMI) && !is(p, EMBI_JST_RBRACE) && !is(p, EMBI_JST_EOF) &&
        !(p->tok.flags & EMBI_JST_F_NL_BEFORE)) {
        expr v;
        if (!parse_expression(p, false, &v)) {
            return false;
        }
        to_value(p, &v);
        if (p->fn->is_async && p->fn->is_gen) {
            OPC(AWAIT);
            OPC32(GEN_RESUME, 0);
        }
    } else {
        OPC(PUSH_UNDEF);
    }
    emit_return_top(p);
    p->fn->depth = depth0;
    return consume_semi(p);
}

static bool parse_throw(cunit *p)
{
    uint32_t line = p->tok.line;
    uint32_t col = p->tok.col;
    adv(p);
    if (p->tok.flags & EMBI_JST_F_NL_BEFORE) {
        return perr(p, "Illegal newline after throw");
    }
    expr v;
    if (!parse_expression(p, false, &v)) {
        return false;
    }
    to_value(p, &v);
    mark(p, line, col);
    OPC(THROW);
    return consume_semi(p);
}

/* The names of a catch pattern exist before the pattern stores them: a default may read them, or
 * capture them in a closure or an eval (the scope resolver boxes them), so each starts in its own
 * box, uninitialized (a temporal dead zone, as the lexical names of a block: block_leave). The
 * binding is checked (ptdz) until its store in the pattern sets init_pc (pat_store). */
static void catch_boxes(cunit *p, cscope *cs)
{
    for (cbind *x = cs->first; x; x = x->next_in_scope) {
        if (x->kind != BK_CATCH) {
            continue;
        }
        OPC(PUSH_EMPTY);
        x->ptdz = true;
        x->init_pc = 0xFFFFFFFFu;
        x->prebox = true;
        x->stub_pc = embi_jsc_here(p);
        embi_jsc_emit_sym(p, SYM_KIND_INIT, x->name, cs, x);
    }
}

static bool parse_try(cunit *p)
{
    adv(p); /* try */
    reset_completion(p);
    uint32_t depth = ctl_items(p);
    cfunc *f = p->fn;

    ctl fin = {nullptr, CTL_TRY_FIN, 0, 0, {0}, nullptr, nullptr, nullptr, nullptr, 2, nullptr, nullptr, false, false, nullptr};
    fin.fk = embi_jsc_new_bind(p, f, nullptr, EMBI_JS_EMPTY, BK_TEMP);
    fin.fv = embi_jsc_new_bind(p, f, nullptr, EMBI_JS_EMPTY, BK_TEMP);
    fin.parent = p->ctl;
    p->ctl = &fin;

    uint32_t start = embi_jsc_here(p);
    if (!parse_block(p)) {
        return false;
    }
    uint32_t end_body = embi_jsc_here(p);

    bool has_catch = false;
    uint32_t j_over = 0;
    uint32_t handler_catch = 0;
    if (is_kw(p, EMBI_JSKW_catch)) {
        has_catch = true;
        adv(p);
        j_over = jump_emit(p, EMBI_JSOP_JMP);
        handler_catch = embi_jsc_here(p);
        cscope *saved_cur = f->cur;
        /* The runtime enters the handler with the exception pushed above the try's base depth. */
        p->fn->depth = (int32_t)depth + 1;
        if (is(p, EMBI_JST_LBRACE)) {
            OPC(POP); /* catch without a binding */
        } else {
            if (!expect(p, EMBI_JST_LPAREN)) {
                return false;
            }
            if (is(p, EMBI_JST_LBRACE) || is(p, EMBI_JST_LBRACK)) {
                /* A pattern: its names live in the catch scope, and the pattern consumes the
                 * exception on the stack. */
                cscope *cs = embi_jsc_new_scope(p, saved_cur);
                cs->kind = SC_CATCH;
                f->cur = cs;
                pat c = {true, BK_CATCH, false, cs};
                if (!pat_declare_names(p, &c)) {
                    return false;
                }
                catch_boxes(p, cs);
                if (!pat_consume(p, &c) || !expect(p, EMBI_JST_RPAREN)) {
                    return false;
                }
            } else {
                if (!is_ident(p)) {
                    return unexpected(p);
                }
                embi_js_value nm = name_atom(p);
                if (!check_strict_name(p, nm)) {
                    return false;
                }
                adv(p);
                if (!expect(p, EMBI_JST_RPAREN)) {
                    return false;
                }
                cscope *cs = embi_jsc_new_scope(p, saved_cur);
                embi_jsc_new_bind(p, f, cs, nm, BK_CATCH);
                cs->kind = SC_CATCH;
                f->cur = cs;
                embi_jsc_emit_sym(p, SYM_KIND_INIT, nm, cs, nullptr);
            }
        }
        if (!parse_block(p)) {
            return false;
        }
        f->cur = saved_cur;
        p->fn->depth = (int32_t)depth;
        embi_js_exc_entry ce = {start, end_body, handler_catch, depth, 0};
        CF_PUSH(f, exc, ce);
    } else if (!is_kw(p, EMBI_JSKW_finally)) {
        return perr(p, "Missing catch or finally after try");
    }

    /* Normal completion (the try block, or the catch block) joins here at the base depth. */
    uint32_t l_set = embi_jsc_here(p);
    if (has_catch) {
        jump_to(p, j_over, l_set);
    }
    p->fn->depth = (int32_t)depth;
    OPC32(PUSH_I32, 0);
    embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, fin.fk);
    uint32_t j_fin = jump_emit(p, EMBI_JSOP_JMP);
    /* An exception: the runtime enters here with the exception pushed above the base depth. */
    uint32_t l_exc = embi_jsc_here(p);
    p->fn->depth = (int32_t)depth + 1;
    embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, fin.fv);
    OPC32(PUSH_I32, 1);
    embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, fin.fk);
    embi_js_exc_entry fe = {start, l_set, l_exc, depth, 1};
    CF_PUSH(f, exc, fe);

    /* The finally block runs outside the protection of its own try. */
    p->ctl = fin.parent;
    embi_jsc_patch_list(p, fin.fin_sites, embi_jsc_here(p));
    embi_jsc_patch(p, j_fin, embi_jsc_here(p));
    p->fn->depth = (int32_t)depth;

    if (is_kw(p, EMBI_JSKW_finally)) {
        adv(p);
        /* A normally completing finally must not change the try/catch completion value. */
        bool keep = has_completion(p->fn);
        cbind *saved = nullptr;
        if (keep) {
            saved = embi_jsc_new_bind(p, f, nullptr, EMBI_JS_EMPTY, BK_TEMP);
            embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, p->fn->completion);
            embi_jsc_emit_sym(p, SYM_KIND_INIT, EMBI_JS_EMPTY, nullptr, saved);
            /* An abrupt finally (break, continue) completes with UpdateEmpty(F, undefined): the
             * try/catch value is only restored when the block ends normally. */
            reset_completion(p);
        }
        if (!parse_block(p)) {
            return false;
        }
        if (keep) {
            embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, saved);
            embi_jsc_emit_sym(p, SYM_KIND_SET, EMBI_JS_EMPTY, nullptr, p->fn->completion);
            OPC(POP);
        }
    }

    /* Dispatch on the completion kind. */
    embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, fin.fk);
    OPC32(PUSH_I32, 1);
    OPC(SEQ);
    uint32_t skip_throw = jump_emit(p, EMBI_JSOP_JMP_FALSE);
    embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, fin.fv);
    OPC(RETHROW);
    jump_to(p, skip_throw, embi_jsc_here(p));
    for (cjumprec *r = fin.jumps; r; r = r->next) {
        embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, fin.fk);
        OPC32(PUSH_I32, r->id);
        OPC(SEQ);
        uint32_t skip = jump_emit(p, EMBI_JSOP_JMP_FALSE);
        if (r->is_return) {
            int32_t depth0 = p->fn->depth;
            embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, fin.fv);
            emit_return_top(p);
            p->fn->depth = depth0; /* the return consumed the value (see parse_return) */
        } else {
            emit_unwind(p, r->target, r->cont, false);
        }
        jump_to(p, skip, embi_jsc_here(p));
    }
    return true;
}

static bool parse_switch(cunit *p)
{
    adv(p); /* switch */
    reset_completion(p);
    if (!expect(p, EMBI_JST_LPAREN)) {
        return false;
    }
    expr d;
    if (!parse_expression(p, false, &d)) {
        return false;
    }
    to_value(p, &d);
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    if (!expect(p, EMBI_JST_LBRACE)) {
        return false;
    }
    /* The case bodies form one block (their function declarations are block-scoped). */
    cblk blk __attribute__((cleanup(blk_free))) = {};
    block_enter(p, &blk, true);
    blk.scope->kind = SC_SWITCH;
    p->block_depth++;
    ctl c = {nullptr, CTL_SWITCH, 1, 0, {0}, nullptr, nullptr, nullptr, nullptr, 2, nullptr, nullptr, false, false, nullptr};
    take_labels(p, &c);
    c.parent = p->ctl;
    p->ctl = &c;

    uint32_t j0 = jump_emit(p, EMBI_JSOP_SWITCH_JMP); /* CF1: runs the constant case tests in one step */
    bool j0_pending = true;
    cpatch *fail = nullptr;
    cpatch *ft = nullptr;
    bool has_default = false;
    uint32_t default_pc = 0;
    while (!is(p, EMBI_JST_RBRACE)) {
        if (is_kw(p, EMBI_JSKW_case)) {
            adv(p);
            if (j0_pending) {
                jump_to(p, j0, embi_jsc_here(p));
                j0_pending = false;
            }
            embi_jsc_patch_list(p, fail, embi_jsc_here(p));
            fail = nullptr;
            OPC(DUP);
            expr t;
            if (!parse_expression(p, false, &t)) {
                return false;
            }
            to_value(p, &t);
            OPC(SEQ);
            uint32_t jt = jump_emit(p, EMBI_JSOP_JMP_TRUE);
            uint32_t jf = jump_emit(p, EMBI_JSOP_JMP);
            fail = embi_jsc_patch_add(p, fail, jf);
            if (!expect(p, EMBI_JST_COLON)) {
                return false;
            }
            embi_jsc_patch_list(p, ft, embi_jsc_here(p));
            ft = nullptr;
            jump_to(p, jt, embi_jsc_here(p));
        } else if (is_kw(p, EMBI_JSKW_default)) {
            adv(p);
            if (has_default) {
                return perr(p, "More than one default clause in switch statement");
            }
            if (!expect(p, EMBI_JST_COLON)) {
                return false;
            }
            has_default = true;
            default_pc = embi_jsc_here(p);
            embi_jsc_patch_list(p, ft, embi_jsc_here(p));
            ft = nullptr;
        } else {
            return unexpected(p);
        }
        while (!is(p, EMBI_JST_RBRACE) && !is_kw(p, EMBI_JSKW_case) &&
               !is_kw(p, EMBI_JSKW_default)) {
            if (is(p, EMBI_JST_EOF)) {
                return unexpected(p);
            }
            if (!parse_statement(p)) {
                return false;
            }
        }
        uint32_t jn = jump_emit(p, EMBI_JSOP_JMP);
        ft = embi_jsc_patch_add(p, ft, jn);
    }
    adv(p); /* } */
    p->block_depth--;

    /* Fall-through of the last body leaves the switch; no match goes to the default body or
     * out. */
    cpatch *to_end = ft;
    ft = nullptr;
    if (j0_pending) {
        jump_to(p, j0, embi_jsc_here(p));
        j0_pending = false;
    }
    embi_jsc_patch_list(p, fail, embi_jsc_here(p));
    fail = nullptr;
    if (has_default) {
        uint32_t jd = jump_emit(p, EMBI_JSOP_JMP);
        embi_jsc_patch(p, jd, default_pc);
    } else {
        uint32_t je = jump_emit(p, EMBI_JSOP_JMP);
        to_end = embi_jsc_patch_add(p, to_end, je);
    }
    embi_jsc_patch_list(p, to_end, embi_jsc_here(p));
    OPC(POP);
    embi_jsc_patch_list(p, c.brk, embi_jsc_here(p));
    p->ctl = c.parent;
    block_leave(p, &blk);
    return true;
}

/* True when the statement at the current token is a loop, or a chain of labels that ends in a
 * loop. Scans ahead with a copy of the lexer. */
static bool label_chain_to_loop(cunit *p)
{
    embi_jstok t = p->tok;
    embi_jslex lx = p->lx;
    lx.scratch = p->peek_scratch;
    lx.scratch_cap = p->peek_cap;
    for (;;) {
        if (t.kind != EMBI_JST_NAME || (t.flags & EMBI_JST_F_ESCAPED)) {
            return false;
        }
        if (t.kw == EMBI_JSKW_for || t.kw == EMBI_JSKW_while || t.kw == EMBI_JSKW_do) {
            return true;
        }
        if (reserved_kw(t.kw)) {
            return false;
        }
        embi_jstok colon;
        lex_scan(p, &lx, &colon, true);
        if (colon.kind != EMBI_JST_COLON) {
            return false;
        }
        lex_scan(p, &lx, &t, true);
    }
}

static bool parse_labeled(cunit *p)
{
    embi_js_value name = name_atom(p);
    adv(p); /* identifier */
    adv(p); /* : */
    if (label_in_use(p, name)) {
        return perr(p, "Label has already been declared");
    }
    for (uint8_t i = 0; i < p->npending; i++) {
        if (p->pending_labels[i] == name) {
            return perr(p, "Label has already been declared");
        }
    }
    if (p->npending >= 8) {
        return perr(p, "Too many nested labels");
    }
    p->pending_labels[p->npending++] = name;
    /* Labels in front of a loop (directly or through other labels) belong to that loop, so
     * continue can name any of them. */
    if (label_chain_to_loop(p)) {
        return parse_statement(p);
    }
    ctl c = {nullptr, CTL_LABEL, 0, 0, {0}, nullptr, nullptr, nullptr, nullptr, 2, nullptr, nullptr, false, false, nullptr};
    take_labels(p, &c);
    c.parent = p->ctl;
    p->ctl = &c;
    /* A labelled function declaration (sloppy code only) is declared where it appears. */
    bool labelled_fn = annexb_function_ahead(p);
    if (!(labelled_fn ? parse_function_decl(p) : parse_sub_statement(p))) {
        return false;
    }
    embi_jsc_patch_list(p, c.brk, embi_jsc_here(p));
    p->ctl = c.parent;
    return true;
}

static bool parse_expr_statement(cunit *p)
{
    expr e;
    if (!parse_expression(p, false, &e)) {
        return false;
    }
    to_value(p, &e);
    if (has_completion(p->fn)) {
        embi_jsc_emit_sym(p, SYM_KIND_SET, EMBI_JS_EMPTY, nullptr, p->fn->completion);
    }
    OPC(POP);
    return consume_semi(p);
}

static bool is_use_strict_directive_here(const cunit *p)
{
    return p->tok.end - p->tok.start == 12 && memcmp(p->src + p->tok.start + 1, "use strict", 10) == 0;
}

/* The with statement. Its body runs in a scope whose only binding holds the with object, so the
 * names it references may resolve to properties of that object at run time. */
static bool parse_with(cunit *p)
{
    if (p->fn->strict) {
        return perr(p, "Strict mode code may not include a with statement");
    }
    adv(p); /* with */
    reset_completion(p);
    if (!expect(p, EMBI_JST_LPAREN)) {
        return false;
    }
    expr o;
    if (!parse_expression(p, false, &o)) {
        return false;
    }
    to_value(p, &o);
    if (!expect(p, EMBI_JST_RPAREN)) {
        return false;
    }
    OPC(TO_OBJECT);
    cfunc *f = p->fn;
    cscope *saved = f->cur;
    cscope *ws = embi_jsc_new_scope(p, saved);
    ws->kind = SC_WITH;
    cbind *wb = embi_jsc_new_bind(p, f, ws, tmp_name(p, 'w'), BK_TEMP);
    embi_jsc_emit_sym(p, SYM_KIND_INIT, wb->name, ws, nullptr);
    f->cur = ws;
    if (!parse_sub_statement(p)) {
        return false;
    }
    f->cur = saved;
    return true;
}

static bool parse_statement(cunit *p)
{
    return parse_statement_ctx(p, false);
}

/* sub: the statement is the body of if/loop/with/label, where `let` is never a declaration (it is
 * an identifier expression statement and ASI applies). */
static bool parse_statement_ctx(cunit *p, bool sub)
{
    NEST_GUARD(p);
    mark(p, p->tok.line, p->tok.col);
    switch (p->tok.kind) {
    case EMBI_JST_LBRACE:
        return parse_block(p);
    case EMBI_JST_SEMI:
        adv(p);
        return true;
    case EMBI_JST_NAME:
        break;
    default:
        return parse_expr_statement(p);
    }
    if (p->tok.flags & EMBI_JST_F_ESCAPED) {
        if (is_ident(p)) {
            embi_jstok nt;
            peek(p, &nt);
            if (nt.kind == EMBI_JST_COLON) {
                return parse_labeled(p);
            }
        }
        return parse_expr_statement(p);
    }
    if (is_kw(p, EMBI_JSKW_async) && async_function_ahead(p)) {
        return parse_function_decl_kw(p, true);
    }
    switch (p->tok.kw) {
    case EMBI_JSKW_var:
        adv(p);
        {
            embi_js_value unused = EMBI_JS_EMPTY;
            if (!parse_var_decls(p, false, false, &unused)) {
                return false;
            }
        }
        return consume_semi(p);
    case EMBI_JSKW_function:
        return parse_function_decl(p);
    case EMBI_JSKW_if:
        return parse_if(p);
    case EMBI_JSKW_while:
        return parse_while(p);
    case EMBI_JSKW_do:
        return parse_do(p);
    case EMBI_JSKW_for:
        return parse_for(p);
    case EMBI_JSKW_continue:
        return parse_break_continue(p, true);
    case EMBI_JSKW_break:
        return parse_break_continue(p, false);
    case EMBI_JSKW_return:
        return parse_return(p);
    case EMBI_JSKW_throw:
        return parse_throw(p);
    case EMBI_JSKW_try:
        return parse_try(p);
    case EMBI_JSKW_switch:
        return parse_switch(p);
    case EMBI_JSKW_with:
        return parse_with(p);
    case EMBI_JSKW_debugger:
        adv(p);
        return consume_semi(p);
    case EMBI_JSKW_let: {
        embi_jstok nt;
        peek(p, &nt);
        if (!sub && let_starts_binding(p)) {
            adv(p);
            embi_js_value unused = EMBI_JS_EMPTY;
            cbind *unused_b = nullptr;
            if (!parse_lexical_decls(p, false, false, false, &unused, &unused_b)) {
                return false;
            }
            return consume_semi(p);
        }
        break;
    }
    case EMBI_JSKW_const: {
        adv(p);
        embi_js_value unused = EMBI_JS_EMPTY;
        cbind *unused_b = nullptr;
        if (!parse_lexical_decls(p, true, false, false, &unused, &unused_b)) {
            return false;
        }
        return consume_semi(p);
    }
    case EMBI_JSKW_class: {
        expr unused;
        return parse_class(p, &unused, true);
    }
    case EMBI_JSKW_import: {
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind == EMBI_JST_LPAREN || nt.kind == EMBI_JST_DOT) {
            return parse_expr_statement(p);
        }
        return perr(p, p->top && p->top->is_module
                           ? "Import declarations may only appear at the top level of a module"
                           : "Cannot use import statement outside a module");
    }
    case EMBI_JSKW_export:
        return perr(p, p->top && p->top->is_module
                           ? "Export declarations may only appear at the top level of a module"
                           : "Unexpected token 'export'");
    case EMBI_JSKW_enum:
        return unexpected(p);
    default:
        break;
    }
    if (is_ident(p)) {
        embi_jstok nt;
        peek(p, &nt);
        if (nt.kind == EMBI_JST_COLON) {
            return parse_labeled(p);
        }
    }
    return parse_expr_statement(p);
}

/* ------------------------------------------------------------------ modules (J3c) */

static bool finish_all(cunit *p);

#include "parser_module.inc"

/* ------------------------------------------------------------------ program */

/* Runs the resolve pass of every function, children before parents (all lists parents first). */
static bool finish_all(cunit *p)
{
    for (uint32_t i = p->all.len; i-- > 0;) {
        if (!embi_jsc_finish_function(p, p->all.data[i])) {
            return false;
        }
    }
    return true;
}

bool embi_jsc_parse_program(cunit *p, bool strict)
{
    cfunc *top = embi_jsc_new_func(p, nullptr, nullptr);
    top->is_script = !p->eval_mode;
    top->is_eval = p->eval_mode;
    top->dynref = p->eval_mode;
    top->strict = strict;
    p->top = top;
    p->fn = top;
    p->ctl = nullptr;
    top->completion = embi_jsc_new_bind(p, top, nullptr, EMBI_JS_EMPTY, BK_TEMP);
    if (p->eval_mode) {
        /* Upvalue 0 holds the caller's context (set when the eval runs). The eval code reads it
         * into a binding at entry, so that its dynamic lookups can reach the caller's scopes. */
        cupval ctx = {EMBI_JS_EMPTY, nullptr, UPV_GLOBAL, nullptr, 0};
        CF_PUSH(top, upvals, ctx);
        top->chain_bind = embi_jsc_new_bind(p, top, top->root, tmp_name(p, 'c'), BK_TEMP);
    }
    OPC32(JMP, 0);
    adv(p);

    if (p->fn_only) {
        /* Exactly "(" function-expression ")" then end of input. The function is parsed by the
         * primary-expression path, so no postfix, operator or second statement can follow it. */
        if (!expect(p, EMBI_JST_LPAREN)) {
            return false;
        }
        uint32_t fstart = p->tok.start;
        bool async = is_kw(p, EMBI_JSKW_async);
        if (async) {
            adv(p);
        }
        if (!is_kw(p, EMBI_JSKW_function)) {
            return unexpected(p);
        }
        adv(p);
        /* function* is the GeneratorFunction source (builtins_iterator.c). */
        bool gen = false;
        if (is(p, EMBI_JST_MUL)) {
            gen = true;
            adv(p);
        }
        embi_js_value fname = EMBI_JS_EMPTY;
        if (p->dynfn && is_ident(p)) {
            fname = name_atom(p);
            p->dynfn_seen = true;
            adv(p);
        }
        cfunc *f;
        p->next_gen = gen;
        p->next_async = async;
        if (!parse_function_common(p, fname, EMBI_JS_EMPTY, fstart, &f)) {
            return false;
        }
        OPC32(NEW_CLOSURE, f->const_idx);
        embi_jsc_emit_sym(p, SYM_KIND_SET, EMBI_JS_EMPTY, nullptr, top->completion);
        OPC(POP);
        if (!expect(p, EMBI_JST_RPAREN)) {
            return false;
        }
        if (!is(p, EMBI_JST_EOF)) {
            return unexpected(p);
        }
    }

    bool directives = true;
    bool octal_directive = false;
    while (!is(p, EMBI_JST_EOF)) {
        if (directives) {
            if (is(p, EMBI_JST_STRING)) {
                embi_jstok nt;
                peek(p, &nt);
                bool ends = directive_ends(&nt);
                if (!ends) {
                    directives = false;
                } else {
                    if (p->tok.flags & EMBI_JST_F_LEGACY) {
                        octal_directive = true;
                    }
                    if (is_use_strict_directive_here(p)) {
                        if (octal_directive) {
                            return perr(p, "Octal escape sequences are not allowed in strict mode");
                        }
                        top->strict = true;
                    }
                }
            } else {
                directives = false;
            }
        }
        if (!parse_statement(p)) {
            return false;
        }
    }
    embi_jsc_emit_sym(p, SYM_KIND_GET, EMBI_JS_EMPTY, nullptr, top->completion);
    OPC(RETURN);
    return finish_all(p);
}

/* FP2a: a source this long is compiled with its arena blocks made (and their pages touched) ahead by a thread: the compile of the
 * react-todomvc bundle used 7.4 MB of arena for 223 KB of source (34 bytes a byte), and the first touch of those pages cost
 * about 2 ms of the cold compile. The thread stops when the compile ends. */
#define EMBI_JSC_PREFAULT_MIN (48u * 1024u)
#define EMBI_JSC_PREFAULT_PER_BYTE 36u

static embi_js_value compile_unit_ex(embi_js_realm *r, const uint8_t *src, uint32_t len,
                                     const char *file, bool strict, bool eval_mode, bool fn_only,
                                     bool dynfn, unsigned eflags, embi_jsc_priv_known_fn priv_known,
                                     void *priv_user, embi_js_value eval_module)
{
    embi_js_runtime *rt = r->rt;
    cunit p;
    memset(&p, 0, sizeof p);
    p.rt = rt;
    p.realm = r;
    p.src = src;
    p.src_len = len;
    p.scratch_cap = len + 1u < 1024u ? len + 1u : 1024u;
    p.peek_cap = p.scratch_cap;
    p.scratch = embi_alloc((size_t)p.scratch_cap * sizeof(uint16_t));
    p.peek_scratch = embi_alloc((size_t)p.peek_cap * sizeof(uint16_t));
    embi_jslex_init(&p.lx, src, len, p.scratch, p.scratch_cap, 0);
    const char *fname = file ? file : "";
    size_t flen = strlen(fname);
    p.file = embi_alloc(flen + 1u);
    memcpy(p.file, fname, flen + 1u);
    embi_arena_init(&p.arena, 32u * 1024u);
    if (len >= EMBI_JSC_PREFAULT_MIN) {
        embi_arena_prefault_start(&p.arena, (size_t)len * EMBI_JSC_PREFAULT_PER_BYTE);
    }
    p.na_start = UINT32_MAX;
    p.hint = EMBI_JS_EMPTY;
    p.eval_mode = eval_mode;
    p.eval_module = eval_module;
    p.fn_only = fn_only;
    p.dynfn = dynfn;
    p.nt_ok = (eflags & EMBI_JSC_EVAL_NT) != 0;
    p.eval_params = (eflags & EMBI_JSC_EVAL_PARAMS) != 0;
    p.eval_super = (eflags & EMBI_JSC_EVAL_SUPER) != 0;
    p.eval_noargs = (eflags & EMBI_JSC_EVAL_NOARGS) != 0;
    p.eval_priv = (eflags & EMBI_JSC_EVAL_PRIV) != 0;
    p.priv_known = priv_known;
    p.priv_user = priv_user;
    p.atom_eval = embi_js_from_string(embi_js_atom_intern_utf8(rt, "eval", 4));
    p.atom_let = embi_js_from_string(embi_js_atom_intern_utf8(rt, "let", 3));
    p.atom_proto = embi_js_from_string(embi_js_atom_intern_utf8(rt, "__proto__", 9));
    p.atom_this = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%this", 5));
    p.atom_nt = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%nt", 3));
    p.atom_home = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%home", 5));
    p.atom_fn = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%fn", 3));
    p.atom_arguments = embi_js_from_string(embi_js_atom_intern_utf8(rt, "arguments", 9));
    static const char *const words[9] = {"implements", "interface", "let",       "package", "private",
                                         "protected",  "public",    "static",    "yield"};
    for (uint32_t i = 0; i < 9; i++) {
        p.atom_strict[i] = embi_js_from_string(embi_js_atom_intern_utf8(rt, words[i], strlen(words[i])));
    }
    const uint64_t t_start = SDL_GetPerformanceCounter();
    embi_js_root_add(rt, embi_jsc_unit_root, &p);
    embi_js_gc_hold_begin(rt);

    bool ok = embi_jsc_parse_program(&p, strict);
    embi_js_value code = EMBI_JS_EXCEPTION;
    if (ok) {
        code = embi_jsc_build(&p);
    }
    embi_js_gc_hold_end(rt);
    embi_js_root_remove(rt, embi_jsc_unit_root, &p);
    embi_jsc_unit_free(&p);
    embi_js_compile_ms_add(rt, SDL_GetPerformanceCounter() - t_start);
    return code;
}

static embi_js_value compile_unit(embi_js_realm *r, const uint8_t *src, uint32_t len,
                                  const char *file, bool strict, bool eval_mode, bool fn_only,
                                  bool dynfn, unsigned eflags)
{
    return compile_unit_ex(r, src, len, file, strict, eval_mode, fn_only, dynfn, eflags, nullptr,
                           nullptr, EMBI_JS_EMPTY);
}

/* Compiles a script. Returns the CODE value, or EMBI_JS_EXCEPTION with the SyntaxError pending. */
embi_js_value embi_js_compile(embi_js_realm *r, const uint8_t *src, uint32_t len, const char *file,
                              bool strict)
{
    return compile_unit(r, src, len, file, strict, false, false, false, 0);
}

embi_js_value embi_js_compile_ex(embi_js_realm *r, const uint8_t *src, uint32_t len, const char *file,
                                 bool strict, bool dynfn)
{
    return compile_unit(r, src, len, file, strict, false, false, dynfn, 0);
}

embi_js_value embi_js_compile_function_only(embi_js_realm *r, const uint8_t *src, uint32_t len)
{
    return compile_unit(r, src, len, "anonymous", false, false, true, true, 0);
}

/* Compiles eval code. The caller's context is supplied at run time (upvalue 0). */
embi_js_value embi_jsc_compile_eval(embi_js_realm *r, const uint8_t *src, uint32_t len, bool strict,
                                    unsigned eflags, embi_jsc_priv_known_fn priv_known,
                                    void *priv_user, embi_js_value module)
{
    return compile_unit_ex(r, src, len, "eval", strict, true, false, false, eflags, priv_known,
                           priv_user, module);
}

/* Compiles a module's source (J3c). mod receives the requests, imports and exports of the module,
 * and holder is the module's holder object (the constants of its code refer to it). Returns the
 * module's CODE, or EMBI_JS_EXCEPTION with the SyntaxError pending. */
embi_js_value embi_jsc_compile_module(embi_js_realm *r, const uint8_t *src, uint32_t len,
                                      const char *file, embi_js_value holder, cmod *mod)
{
    embi_js_runtime *rt = r->rt;
    cunit p;
    memset(&p, 0, sizeof p);
    p.rt = rt;
    p.realm = r;
    p.src = src;
    p.src_len = len;
    p.scratch_cap = len + 1u < 1024u ? len + 1u : 1024u;
    p.peek_cap = p.scratch_cap;
    p.scratch = embi_alloc((size_t)p.scratch_cap * sizeof(uint16_t));
    p.peek_scratch = embi_alloc((size_t)p.peek_cap * sizeof(uint16_t));
    embi_jslex_init(&p.lx, src, len, p.scratch, p.scratch_cap, EMBI_JSLEX_MODULE);
    const char *fname = file ? file : "";
    size_t flen = strlen(fname);
    p.file = embi_alloc(flen + 1u);
    memcpy(p.file, fname, flen + 1u);
    embi_arena_init(&p.arena, 32u * 1024u);
    if (len >= EMBI_JSC_PREFAULT_MIN) {
        embi_arena_prefault_start(&p.arena, (size_t)len * EMBI_JSC_PREFAULT_PER_BYTE);
    }
    p.na_start = UINT32_MAX;
    p.hint = EMBI_JS_EMPTY;
    p.mod = mod;
    p.holder = holder;
    p.atom_eval = embi_js_from_string(embi_js_atom_intern_utf8(rt, "eval", 4));
    p.atom_let = embi_js_from_string(embi_js_atom_intern_utf8(rt, "let", 3));
    p.atom_proto = embi_js_from_string(embi_js_atom_intern_utf8(rt, "__proto__", 9));
    p.atom_this = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%this", 5));
    p.atom_nt = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%nt", 3));
    p.atom_home = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%home", 5));
    p.atom_fn = embi_js_from_string(embi_js_atom_intern_utf8(rt, "%fn", 3));
    p.atom_arguments = embi_js_from_string(embi_js_atom_intern_utf8(rt, "arguments", 9));
    static const char *const words[9] = {"implements", "interface", "let",       "package", "private",
                                         "protected",  "public",    "static",    "yield"};
    for (uint32_t i = 0; i < 9; i++) {
        p.atom_strict[i] = embi_js_from_string(embi_js_atom_intern_utf8(rt, words[i], strlen(words[i])));
    }
    const uint64_t t_start = SDL_GetPerformanceCounter();
    embi_js_root_add(rt, embi_jsc_unit_root, &p);
    embi_js_gc_hold_begin(rt);

    bool ok = embi_jsc_parse_module(&p);
    embi_js_value code = EMBI_JS_EXCEPTION;
    if (ok) {
        code = embi_jsc_build(&p);
        /* The storage of the local exports is known now: each export names its box's slot. */
        for (uint32_t i = 0; i < mod->exps.len; i++) {
            cmexp *e = &mod->exps.data[i];
            if (e->kind == CMEX_LOCAL) {
                const cbind *b = embi_jsc_find_in_scope(p.top->root, e->local);
                e->slot = b ? b->slot : -1;
            }
        }
    }
    embi_js_gc_hold_end(rt);
    embi_js_root_remove(rt, embi_jsc_unit_root, &p);
    embi_jsc_unit_free(&p);
    embi_js_compile_ms_add(rt, SDL_GetPerformanceCounter() - t_start);
    return code;
}
