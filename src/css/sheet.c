/*
 * Stylesheet model (W2a section 2). Turns the syntax items of css/syntax.c into rules.
 *
 * Context rules: @import and @namespace are only valid at the top of a sheet, and @import must
 * come before everything except @charset and layer statements. Inside a style rule, only the
 * conditional at-rules, @layer and @scope may nest. The rest is dropped per spec.
 *
 * Declarations are kept as raw component-value slices. Validation goes through the property hooks
 * (W2c). Descriptors of @font-face and @property are never validated here.
 */
#include "css/css.h"
#include "css/selector.h"

#include <string.h>

#define SHEET_MAX_DEPTH 128
#define SHEET_MAX_IMPORTS 32
#define SHEET_MAX_TOTAL_IMPORTS 256 /* distinct imported sheets per root stylesheet */
#define SHEET_MAX_EVAL_DEPTH 64

typedef enum ctx_kind {
    CTX_TOP,   /* top level of a sheet */
    CTX_RULES, /* rule list inside a conditional or layer block at any level */
    CTX_STYLE, /* block contents of a style rule (declarations and nested rules) */
} ctx_kind;

typedef EMBI_VEC(embi_css_rule) rule_vec;
typedef EMBI_VEC(embi_css_decl) decl_vec;
typedef EMBI_VEC(embi_atom) atom_vec;
typedef EMBI_VEC(embi_str) str_vec;

typedef struct import_cache_ent {
    char *url; /* resolved URL, owned */
    uint32_t len;
    embi_css_sheet *sheet; /* holds one reference */
} import_cache_ent;

/* State shared by a root stylesheet and every sheet it imports, directly or not. */
typedef struct import_state {
    str_vec active; /* URLs of the imports being parsed, for cycle detection */
    EMBI_VEC(import_cache_ent) cache; /* finished imports, shared by resolved URL */
    uint32_t total;                   /* sheets fetched and parsed so far */
} import_state;

static void import_state_free(import_state *st)
{
    for (uint32_t k = 0; k < st->cache.len; k++) {
        embi_css_sheet_unref(st->cache.data[k].sheet);
        embi_free(st->cache.data[k].url, st->cache.data[k].len + 1);
    }
    embi_vec_free(&st->cache);
    embi_vec_free(&st->active);
}

typedef struct builder {
    embi_arena *a;
    const embi_css_parse_opts *opts;
    embi_str base;
    import_state *st;
    embi_sb scratch;
    uint32_t depth;
} builder;

typedef struct list_state {
    bool import_ok;    /* @import still allowed */
    bool namespace_ok; /* @namespace still allowed */
} list_state;

static const embi_css_parse_opts no_opts;

static void finish_decls(embi_arena *a, decl_vec *v, embi_css_decl **p, uint32_t *n)
{
    *n = v->len;
    *p = embi_css_arena_copy(a, v->data, v->len, sizeof(embi_css_decl));
    embi_vec_free(v);
}

static void finish_rules(embi_arena *a, rule_vec *v, embi_css_rule **p, uint32_t *n)
{
    *n = v->len;
    *p = embi_css_arena_copy(a, v->data, v->len, sizeof(embi_css_rule));
    embi_vec_free(v);
}

static void finish_atoms(embi_arena *a, atom_vec *v, embi_atom **p, uint32_t *n)
{
    *n = v->len;
    *p = embi_css_arena_copy(a, v->data, v->len, sizeof(embi_atom));
    embi_vec_free(v);
}

/* ------------------------------------------------------------------ small helpers */

static bool is_ws(const embi_css_cv *c)
{
    return c->tok.type == EMBI_CSS_WS;
}

static bool is_type(const embi_css_cv *c, embi_css_type t)
{
    return c->tok.type == t;
}

static bool is_ident_ci(const embi_css_cv *c, const char *lit)
{
    return c->tok.type == EMBI_CSS_IDENT &&
           embi_str_ieq(c->tok.value, (embi_str){lit, (uint32_t)strlen(lit)});
}

static bool is_fn_ci(const embi_css_cv *c, const char *lit)
{
    return c->tok.type == EMBI_CSS_FUNCTION &&
           embi_str_ieq(c->tok.value, (embi_str){lit, (uint32_t)strlen(lit)});
}

static bool is_delim(const embi_css_cv *c, uint32_t cp)
{
    return c->tok.type == EMBI_CSS_DELIM && c->tok.cp == cp;
}

static uint32_t skip_ws(embi_css_slice in, uint32_t i)
{
    while (i < in.n && is_ws(&in.p[i])) {
        i++;
    }
    return i;
}

static embi_css_slice sub(embi_css_slice in, uint32_t a, uint32_t b)
{
    return embi_css_slice_make(in.p + a, b - a);
}

static embi_css_slice block_of(const embi_css_cv *c)
{
    return embi_css_slice_make(c->kids, c->nkids);
}

static bool only_ws(embi_css_slice in)
{
    return skip_ws(in, 0) >= in.n;
}

/* Normalized property name: ASCII-lowercased, except for custom properties. */
static embi_atom prop_atom(builder *b, embi_str name)
{
    if (name.len >= 2 && name.ptr[0] == '-' && name.ptr[1] == '-') {
        return embi_atom_intern(name.ptr, name.len);
    }
    embi_sb_clear(&b->scratch);
    for (uint32_t k = 0; k < name.len; k++) {
        char c = name.ptr[k];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        embi_sb_append_char(&b->scratch, c);
    }
    return embi_atom_intern(b->scratch.data, b->scratch.len);
}

static bool make_decl(builder *b, const embi_css_item *it, bool validate, embi_css_decl *d)
{
    d->name = prop_atom(b, it->name);
    d->prop = 0;
    d->important = it->important;
    d->value = it->prelude;
    d->order = it->start;
    if (validate) {
        const embi_css_hooks *h = b->opts->hooks;
        if (h != nullptr && h->decl_ok != nullptr &&
            !h->decl_ok(h->user, embi_atom_str(d->name), it->prelude, it->important)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ @supports conditions */

static int eval_cond(builder *b, embi_css_slice in, uint32_t depth);

/* Evaluates a parenthesized group: a declaration, or a nested condition, or general-enclosed. */
static int eval_paren(builder *b, embi_css_slice in, uint32_t depth)
{
    uint32_t i = skip_ws(in, 0);
    if (i >= in.n) {
        return 0;
    }
    if (is_type(&in.p[i], EMBI_CSS_IDENT)) {
        uint32_t j = skip_ws(in, i + 1);
        if (j < in.n && is_type(&in.p[j], EMBI_CSS_COLON)) {
            embi_css_item it = embi_css_parse_declaration(b->a, in);
            if (it.kind != EMBI_CSS_ITEM_DECL) {
                return 0;
            }
            const embi_css_hooks *h = b->opts->hooks;
            if (h == nullptr || h->decl_ok == nullptr) {
                return 0;
            }
            embi_atom name = prop_atom(b, it.name);
            return h->decl_ok(h->user, embi_atom_str(name), it.prelude, it.important) ? 1 : 0;
        }
    }
    int r = eval_cond(b, in, depth + 1);
    return r < 0 ? 0 : r;
}

/* One <supports-in-parens> or selector() at *ip. Returns 1 or 0, or -1 for invalid syntax. */
static int eval_term(builder *b, embi_css_slice in, uint32_t *ip, uint32_t depth)
{
    uint32_t i = *ip;
    if (i >= in.n) {
        return -1;
    }
    const embi_css_cv *c = &in.p[i];
    if (is_type(c, EMBI_CSS_LPAREN)) {
        *ip = i + 1;
        return eval_paren(b, block_of(c), depth);
    }
    if (c->tok.type == EMBI_CSS_FUNCTION) {
        *ip = i + 1;
        if (is_fn_ci(c, "selector")) {
            const embi_css_hooks *h = b->opts->hooks;
            if (h != nullptr && h->selector_ok != nullptr && h->selector_ok(h->user, block_of(c))) {
                return 1;
            }
            return 0;
        }
        return 0; /* general-enclosed */
    }
    return -1;
}

/* Evaluates a whole <supports-condition>. Returns 1, 0, or -1 for invalid syntax. */
static int eval_cond(builder *b, embi_css_slice in, uint32_t depth)
{
    if (depth > SHEET_MAX_EVAL_DEPTH) {
        return -1;
    }
    uint32_t i = skip_ws(in, 0);
    if (i >= in.n) {
        return -1;
    }
    if (is_ident_ci(&in.p[i], "not")) {
        i = skip_ws(in, i + 1);
        int r = eval_term(b, in, &i, depth + 1);
        if (r < 0 || skip_ws(in, i) != in.n) {
            return -1;
        }
        return !r;
    }
    int r = eval_term(b, in, &i, depth + 1);
    if (r < 0) {
        return -1;
    }
    int op = 0; /* 1 = and, 2 = or */
    for (;;) {
        i = skip_ws(in, i);
        if (i >= in.n) {
            return r;
        }
        int this_op;
        if (is_ident_ci(&in.p[i], "and")) {
            this_op = 1;
        } else if (is_ident_ci(&in.p[i], "or")) {
            this_op = 2;
        } else {
            return -1;
        }
        if (op != 0 && op != this_op) {
            return -1;
        }
        op = this_op;
        i = skip_ws(in, i + 1);
        int r2 = eval_term(b, in, &i, depth + 1);
        if (r2 < 0) {
            return -1;
        }
        r = op == 1 ? (r && r2) : (r || r2);
    }
}

/* ------------------------------------------------------------------ @layer names, @import */

/* Parses a comma-separated list of dotted layer names. Returns false on bad syntax. An empty list
 * is valid and leaves names empty. */
static bool parse_layer_names(builder *b, embi_css_slice in, atom_vec *names)
{
    uint32_t i = skip_ws(in, 0);
    if (i >= in.n) {
        return true;
    }
    for (;;) {
        if (i >= in.n || !is_type(&in.p[i], EMBI_CSS_IDENT)) {
            return false;
        }
        embi_sb_clear(&b->scratch);
        embi_sb_append(&b->scratch, in.p[i].tok.value);
        i++;
        while (i < in.n && is_delim(&in.p[i], '.')) {
            i++;
            if (i >= in.n || !is_type(&in.p[i], EMBI_CSS_IDENT)) {
                return false;
            }
            embi_sb_append_char(&b->scratch, '.');
            embi_sb_append(&b->scratch, in.p[i].tok.value);
            i++;
        }
        embi_vec_push(names, embi_atom_intern(b->scratch.data, b->scratch.len));
        i = skip_ws(in, i);
        if (i >= in.n) {
            return true;
        }
        if (!is_type(&in.p[i], EMBI_CSS_COMMA)) {
            return false;
        }
        i = skip_ws(in, i + 1);
        if (i >= in.n) {
            return false;
        }
    }
}

/* A string or url() value at *ip, for @import and @namespace. */
static bool take_url(embi_css_slice in, uint32_t *ip, embi_str *url)
{
    uint32_t i = *ip;
    if (i >= in.n) {
        return false;
    }
    const embi_css_cv *c = &in.p[i];
    if (is_type(c, EMBI_CSS_STRING) || is_type(c, EMBI_CSS_URL)) {
        *url = c->tok.value;
        *ip = i + 1;
        return true;
    }
    if (is_fn_ci(c, "url")) {
        embi_css_slice k = block_of(c);
        uint32_t j = skip_ws(k, 0);
        if (j < k.n && is_type(&k.p[j], EMBI_CSS_STRING)) {
            *url = k.p[j].tok.value;
            *ip = i + 1;
            return true;
        }
    }
    return false;
}

static embi_css_sheet *sheet_fill_text(embi_css_sheet *s, embi_str text,
                                       const embi_css_parse_opts *opts, import_state *st,
                                       embi_str base);

static embi_str arena_str_copy(embi_arena *a, embi_str s)
{
    char *p = embi_arena_alloc(a, s.len + 1, 1);
    if (s.len != 0) {
        memcpy(p, s.ptr, s.len);
    }
    p[s.len] = '\0';
    return (embi_str){p, s.len};
}

/* Fetches and parses an @import target. The loader resolves the URL, and cycle detection compares
 * the resolved form. Returns nullptr for a missing loader, a failed fetch, an unsupported
 * encoding, or a cycle. The resolved URL is copied into the importing sheet's arena, so it stays
 * valid while the child parses and while the importing sheet lives. */
static embi_css_sheet *import_fetch(builder *b, embi_str url)
{
    const embi_css_loader *ld = b->opts->loader;
    if (ld == nullptr || ld->fetch == nullptr || b->st->active.len >= SHEET_MAX_IMPORTS ||
        b->st->total >= SHEET_MAX_TOTAL_IMPORTS) {
        return nullptr;
    }
    embi_str resolved = {nullptr, 0};
    const unsigned char *bytes = nullptr;
    size_t n = 0;
    if (!ld->fetch(ld->user, b->base, url, &resolved, &bytes, &n)) {
        return nullptr;
    }
    embi_str key = arena_str_copy(b->a, resolved.len != 0 ? resolved : url);
    import_state *st = b->st;
    for (uint32_t k = 0; k < st->active.len; k++) {
        if (embi_str_eq(st->active.data[k], key)) {
            return nullptr;
        }
    }
    /* The same resolved URL is parsed once per root: later imports share the sheet. */
    for (uint32_t k = 0; k < st->cache.len; k++) {
        if (st->cache.data[k].len == key.len &&
            memcmp(st->cache.data[k].url, key.ptr, key.len) == 0) {
            embi_css_sheet_ref(st->cache.data[k].sheet);
            return st->cache.data[k].sheet;
        }
    }
    st->total++;
    embi_vec_push(&st->active, key);

    embi_css_sheet *child = embi_calloc(1, sizeof *child);
    atomic_store_explicit(&child->refs, 1, memory_order_relaxed);
    embi_arena_init(&child->arena, 16384);
    embi_str text;
    const char *enc = nullptr;
    if (!embi_css_decode_bytes(&child->arena, bytes, n, (embi_str){nullptr, 0},
                               (embi_str){nullptr, 0}, &text, &enc)) {
        st->active.len--;
        embi_css_sheet_unref(child);
        return nullptr;
    }
    sheet_fill_text(child, text, b->opts, st, key);
    st->active.len--;
    import_cache_ent ent = {embi_alloc(key.len + 1), key.len, child};
    memcpy(ent.url, key.ptr, key.len);
    ent.url[key.len] = '\0';
    embi_css_sheet_ref(child);
    embi_vec_push(&st->cache, ent);
    return child;
}

static bool build_import(builder *b, const embi_css_item *it, embi_css_rule *out)
{
    embi_css_slice in = it->prelude;
    uint32_t i = skip_ws(in, 0);
    embi_str url;
    if (!take_url(in, &i, &url)) {
        return false;
    }
    i = skip_ws(in, i);
    out->url = url;
    out->cond = true;
    if (i < in.n && is_ident_ci(&in.p[i], "layer")) {
        out->has_layer = true;
        i = skip_ws(in, i + 1);
    } else if (i < in.n && is_fn_ci(&in.p[i], "layer")) {
        atom_vec names = {0};
        bool ok = parse_layer_names(b, block_of(&in.p[i]), &names);
        ok = ok && names.len == 1;
        if (ok) {
            out->has_layer = true;
            out->name = names.data[0];
        }
        embi_vec_free(&names);
        if (!ok) {
            return false;
        }
        i = skip_ws(in, i + 1);
    }
    if (i < in.n && is_fn_ci(&in.p[i], "supports")) {
        /* supports() on @import takes a <supports-condition> or a bare <declaration>. */
        out->cond = eval_paren(b, block_of(&in.p[i]), 0) == 1;
        i = skip_ws(in, i + 1);
    }
    out->prelude = sub(in, i, in.n);
    /* A false supports() condition means the import is neither fetched nor applied. */
    out->sheet = out->cond ? import_fetch(b, url) : nullptr;
    return true;
}

static bool build_namespace(builder *b, const embi_css_item *it, embi_css_rule *out)
{
    embi_css_slice in = it->prelude;
    uint32_t i = skip_ws(in, 0);
    if (i < in.n && is_type(&in.p[i], EMBI_CSS_IDENT)) {
        out->name = embi_atom_intern(in.p[i].tok.value.ptr, in.p[i].tok.value.len);
        i = skip_ws(in, i + 1);
    }
    embi_str url;
    if (!take_url(in, &i, &url)) {
        return false;
    }
    if (skip_ws(in, i) < in.n) {
        return false;
    }
    out->url = url;
    (void)b;
    return true;
}

/* ------------------------------------------------------------------ rule building */

static bool build_rule(builder *b, const embi_css_item *it, ctx_kind ctx, list_state *ls,
                       embi_css_rule *out);

static void build_rules_into(builder *b, embi_css_items items, ctx_kind ctx, rule_vec *rv)
{
    list_state ls = {true, true};
    for (uint32_t k = 0; k < items.n; k++) {
        embi_css_rule nr;
        if (build_rule(b, &items.p[k], ctx, &ls, &nr)) {
            embi_vec_push(rv, nr);
        }
    }
}

/* Block contents of a style rule: declarations, then nested rules, in source order. */
static void build_style_body(builder *b, embi_css_slice block, embi_css_rule *r)
{
    embi_css_items items = embi_css_consume_block_contents(b->a, block);
    decl_vec dv = {0};
    rule_vec rv = {0};
    list_state ls = {true, true};
    for (uint32_t k = 0; k < items.n; k++) {
        const embi_css_item *it = &items.p[k];
        if (it->kind == EMBI_CSS_ITEM_DECL) {
            embi_css_decl d;
            if (make_decl(b, it, true, &d)) {
                embi_vec_push(&dv, d);
            }
            continue;
        }
        embi_css_rule nr;
        if (build_rule(b, it, CTX_STYLE, &ls, &nr)) {
            embi_vec_push(&rv, nr);
        }
    }
    finish_decls(b->a, &dv, &r->decls, &r->ndecls);
    finish_rules(b->a, &rv, &r->kids, &r->nkids);
}

/* Body of a conditional or layer block: block contents when nested in a style rule, else a rule
 * list. */
static void build_cond_body(builder *b, embi_css_slice block, ctx_kind ctx, embi_css_rule *r)
{
    if (ctx == CTX_STYLE) {
        build_style_body(b, block, r);
        return;
    }
    rule_vec rv = {0};
    build_rules_into(b, embi_css_consume_rules(b->a, block, false), CTX_RULES, &rv);
    finish_rules(b->a, &rv, &r->kids, &r->nkids);
}

static void close_windows(list_state *ls)
{
    ls->import_ok = false;
    ls->namespace_ok = false;
}

/* CSS-wide keywords and "default" are not custom idents (CSS Values 4), so they cannot name a
 * keyframes rule. "none" is excluded for the same reason, but only as an ident: a string "none"
 * is a valid name. */
static bool reserved_ident(embi_str s)
{
    static const char *const words[] = {"none",   "initial", "inherit",     "unset",
                                        "revert", "revert-layer", "default"};
    for (size_t k = 0; k < sizeof words / sizeof words[0]; k++) {
        if (embi_str_ieq(s, (embi_str){words[k], (uint32_t)strlen(words[k])})) {
            return true;
        }
    }
    return false;
}

/* A keyframe selector list: comma-separated from, to or percentages. */
static bool valid_keyframe_selector(embi_css_slice in)
{
    bool want_item = true;
    bool any = false;
    for (uint32_t i = 0; i < in.n; i++) {
        const embi_css_cv *c = &in.p[i];
        if (is_ws(c)) {
            continue;
        }
        if (want_item) {
            bool ok = (is_type(c, EMBI_CSS_PERCENTAGE) && c->tok.num >= 0 && c->tok.num <= 100) ||
                      (is_type(c, EMBI_CSS_IDENT) &&
                       (embi_str_ieq(c->tok.value, EMBI_STR("from")) ||
                        embi_str_ieq(c->tok.value, EMBI_STR("to"))));
            if (!ok) {
                return false;
            }
            want_item = false;
            any = true;
        } else {
            if (!is_type(c, EMBI_CSS_COMMA)) {
                return false;
            }
            want_item = true;
        }
    }
    return any && !want_item;
}

static bool build_keyframes(builder *b, const embi_css_item *it, embi_css_rule *out)
{
    embi_css_slice in = it->prelude;
    uint32_t i = skip_ws(in, 0);
    if (i >= in.n || !(is_type(&in.p[i], EMBI_CSS_IDENT) || is_type(&in.p[i], EMBI_CSS_STRING))) {
        return false;
    }
    if (is_type(&in.p[i], EMBI_CSS_IDENT) && reserved_ident(in.p[i].tok.value)) {
        return false;
    }
    out->name = embi_atom_intern(in.p[i].tok.value.ptr, in.p[i].tok.value.len);
    if (skip_ws(in, i + 1) < in.n) {
        return false;
    }

    embi_css_items items = embi_css_consume_rules(b->a, it->block, false);
    rule_vec rv = {0};
    for (uint32_t k = 0; k < items.n; k++) {
        const embi_css_item *kf = &items.p[k];
        if (kf->kind != EMBI_CSS_ITEM_QUALIFIED) {
            continue;
        }
        if (!valid_keyframe_selector(kf->prelude)) {
            continue;
        }
        embi_css_rule kr;
        memset(&kr, 0, sizeof kr);
        kr.kind = EMBI_CSS_RULE_STYLE;
        kr.order = kf->start;
        kr.prelude = kf->prelude;
        decl_vec dv = {0};
        embi_css_items body = embi_css_consume_declarations(b->a, kf->block);
        for (uint32_t d = 0; d < body.n; d++) {
            /* !important declarations in a keyframe are ignored (CSS Animations). */
            if (body.p[d].kind == EMBI_CSS_ITEM_DECL && !body.p[d].important) {
                embi_css_decl dd;
                if (make_decl(b, &body.p[d], false, &dd)) {
                    embi_vec_push(&dv, dd);
                }
            }
        }
        finish_decls(b->a, &dv, &kr.decls, &kr.ndecls);
        embi_vec_push(&rv, kr);
    }
    finish_rules(b->a, &rv, &out->kids, &out->nkids);
    return true;
}

static bool build_rule_body(builder *b, const embi_css_item *it, ctx_kind ctx, list_state *ls,
                            embi_css_rule *out)
{
    if (it->kind == EMBI_CSS_ITEM_QUALIFIED) {
        /* A style rule with an invalid selector is dropped when the sheet is parsed, so it does not end the place where
         * @import and @namespace may stand (CSS Syntax 3 section 9). */
        if (ctx == CTX_TOP && ls->import_ok && embi_sel_parse(b->a, it->prelude, nullptr) == nullptr) {
            return false;
        }
        /* A nested block with no selector at all is not a rule (CSS Nesting: the prelude is a relative selector list). */
        if (ctx == CTX_STYLE && skip_ws(it->prelude, 0) >= it->prelude.n) {
            return false;
        }
        out->kind = EMBI_CSS_RULE_STYLE;
        out->prelude = it->prelude;
        build_style_body(b, it->block, out);
        close_windows(ls);
        return true;
    }
    if (it->kind != EMBI_CSS_ITEM_AT) {
        return false;
    }
    const embi_str name = it->name;

    if (embi_str_ieq(name, EMBI_STR("import"))) {
        if (ctx != CTX_TOP || !ls->import_ok || it->has_block) {
            return false;
        }
        out->kind = EMBI_CSS_RULE_IMPORT;
        return build_import(b, it, out);
    }
    if (embi_str_ieq(name, EMBI_STR("namespace"))) {
        if (ctx != CTX_TOP || !ls->namespace_ok || it->has_block) {
            return false;
        }
        out->kind = EMBI_CSS_RULE_NAMESPACE;
        if (!build_namespace(b, it, out)) {
            return false;
        }
        ls->import_ok = false;
        return true;
    }
    if (embi_str_ieq(name, EMBI_STR("layer"))) {
        atom_vec names = {0};
        bool ok = parse_layer_names(b, it->prelude, &names);
        if (!ok) {
            embi_vec_free(&names);
            return false;
        }
        out->kind = EMBI_CSS_RULE_LAYER;
        if (!it->has_block) {
            /* The statement form is not a valid nested group rule. */
            if (names.len == 0 || ctx == CTX_STYLE) {
                embi_vec_free(&names);
                return false;
            }
            finish_atoms(b->a, &names, &out->layers, &out->nlayers);
            return true;
        }
        if (names.len > 1) {
            embi_vec_free(&names);
            return false;
        }
        out->block = true;
        out->name = names.len == 1 ? names.data[0] : 0;
        embi_vec_free(&names);
        build_cond_body(b, it->block, ctx, out);
        close_windows(ls);
        return true;
    }

    bool is_media = embi_str_ieq(name, EMBI_STR("media"));
    bool is_supports = embi_str_ieq(name, EMBI_STR("supports"));
    bool is_container = embi_str_ieq(name, EMBI_STR("container"));
    bool is_scope = embi_str_ieq(name, EMBI_STR("scope"));
    if (is_media || is_supports || is_container || is_scope) {
        if (!it->has_block) {
            return false;
        }
        out->prelude = it->prelude;
        if (is_media) {
            out->kind = EMBI_CSS_RULE_MEDIA;
        } else if (is_supports) {
            out->kind = EMBI_CSS_RULE_SUPPORTS;
            int r = eval_cond(b, it->prelude, 0);
            if (r < 0) {
                return false; /* invalid condition: the whole rule is dropped */
            }
            out->cond = r == 1;
        } else if (is_container) {
            out->kind = EMBI_CSS_RULE_CONTAINER;
            uint32_t i = skip_ws(it->prelude, 0);
            if (i < it->prelude.n && is_type(&it->prelude.p[i], EMBI_CSS_IDENT) &&
                !embi_str_ieq(it->prelude.p[i].tok.value, EMBI_STR("not")) &&
                !embi_str_ieq(it->prelude.p[i].tok.value, EMBI_STR("and")) &&
                !embi_str_ieq(it->prelude.p[i].tok.value, EMBI_STR("or"))) {
                uint32_t j = skip_ws(it->prelude, i + 1);
                if (j < it->prelude.n && (is_type(&it->prelude.p[j], EMBI_CSS_LPAREN) ||
                                          it->prelude.p[j].tok.type == EMBI_CSS_FUNCTION)) {
                    out->name = embi_atom_intern(it->prelude.p[i].tok.value.ptr,
                                                 it->prelude.p[i].tok.value.len);
                    out->prelude = sub(it->prelude, j, it->prelude.n);
                }
            }
        } else {
            out->kind = EMBI_CSS_RULE_SCOPE;
        }
        build_cond_body(b, it->block, ctx, out);
        close_windows(ls);
        return true;
    }

    if (embi_str_ieq(name, EMBI_STR("font-face"))) {
        if (ctx == CTX_STYLE || !it->has_block || !only_ws(it->prelude)) {
            return false;
        }
        out->kind = EMBI_CSS_RULE_FONT_FACE;
        decl_vec dv = {0};
        embi_css_items body = embi_css_consume_declarations(b->a, it->block);
        for (uint32_t k = 0; k < body.n; k++) {
            if (body.p[k].kind == EMBI_CSS_ITEM_DECL) {
                embi_css_decl d;
                if (make_decl(b, &body.p[k], false, &d)) {
                    embi_vec_push(&dv, d);
                }
            }
        }
        finish_decls(b->a, &dv, &out->decls, &out->ndecls);
        close_windows(ls);
        return true;
    }

    if (embi_str_ieq(name, EMBI_STR("counter-style"))) {
        /* CSS Counter Styles 3 section 3: @counter-style <counter-style-name> { descriptors }. The name is a
         * <custom-ident> other than none; decimal, disc, square, circle, disclosure-open and disclosure-closed cannot
         * be redefined. */
        if (ctx == CTX_STYLE || !it->has_block) {
            return false;
        }
        embi_css_slice in = it->prelude;
        uint32_t i = skip_ws(in, 0);
        if (i >= in.n || !is_type(&in.p[i], EMBI_CSS_IDENT) || skip_ws(in, i + 1) < in.n) {
            return false;
        }
        embi_str cname = in.p[i].tok.value;
        static const char *const fixed[] = {"none", "decimal", "disc", "square", "circle", "disclosure-open",
                                            "disclosure-closed", "inherit", "initial", "unset", "default",
                                            "revert", "revert-layer"};
        for (uint32_t k = 0; k < sizeof fixed / sizeof fixed[0]; k++) {
            if (embi_str_ieq(cname, (embi_str){fixed[k], (uint32_t)strlen(fixed[k])})) {
                return false;
            }
        }
        out->kind = EMBI_CSS_RULE_COUNTER_STYLE;
        out->name = embi_atom_intern(cname.ptr, cname.len);
        decl_vec dv = {0};
        embi_css_items body = embi_css_consume_declarations(b->a, it->block);
        for (uint32_t k = 0; k < body.n; k++) {
            if (body.p[k].kind == EMBI_CSS_ITEM_DECL) {
                embi_css_decl d;
                if (make_decl(b, &body.p[k], false, &d)) {
                    embi_vec_push(&dv, d);
                }
            }
        }
        finish_decls(b->a, &dv, &out->decls, &out->ndecls);
        close_windows(ls);
        return true;
    }

    if (embi_str_ieq(name, EMBI_STR("keyframes"))) {
        if (ctx == CTX_STYLE || !it->has_block) {
            return false;
        }
        out->kind = EMBI_CSS_RULE_KEYFRAMES;
        if (!build_keyframes(b, it, out)) {
            return false;
        }
        close_windows(ls);
        return true;
    }

    if (embi_str_ieq(name, EMBI_STR("property"))) {
        if (ctx == CTX_STYLE || !it->has_block) {
            return false;
        }
        embi_css_slice in = it->prelude;
        uint32_t i = skip_ws(in, 0);
        if (i >= in.n || !is_type(&in.p[i], EMBI_CSS_IDENT)) {
            return false;
        }
        embi_str pname = in.p[i].tok.value;
        if (pname.len < 3 || pname.ptr[0] != '-' || pname.ptr[1] != '-') {
            return false;
        }
        if (skip_ws(in, i + 1) < in.n) {
            return false;
        }
        out->kind = EMBI_CSS_RULE_PROPERTY;
        out->name = embi_atom_intern(pname.ptr, pname.len);
        decl_vec dv = {0};
        embi_css_items body = embi_css_consume_declarations(b->a, it->block);
        for (uint32_t k = 0; k < body.n; k++) {
            if (body.p[k].kind == EMBI_CSS_ITEM_DECL) {
                embi_css_decl d;
                if (make_decl(b, &body.p[k], false, &d)) {
                    embi_vec_push(&dv, d);
                }
            }
        }
        finish_decls(b->a, &dv, &out->decls, &out->ndecls);
        close_windows(ls);
        return true;
    }

    /* Unknown at-rules (and @charset, which only the byte decoder uses) are dropped. */
    return false;
}

static bool build_rule(builder *b, const embi_css_item *it, ctx_kind ctx, list_state *ls,
                       embi_css_rule *out)
{
    if (it->kind == EMBI_CSS_ITEM_ERROR || it->kind == EMBI_CSS_ITEM_DECL) {
        return false;
    }
    if (b->depth >= SHEET_MAX_DEPTH) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->order = it->start;
    b->depth++;
    bool ok = build_rule_body(b, it, ctx, ls, out);
    b->depth--;
    return ok;
}

/* ------------------------------------------------------------------ sheet construction */

static embi_css_sheet *sheet_fill_text(embi_css_sheet *s, embi_str text,
                                       const embi_css_parse_opts *opts, import_state *st,
                                       embi_str base)
{
    builder b;
    memset(&b, 0, sizeof b);
    b.a = &s->arena;
    b.opts = opts;
    b.base = base;
    b.st = st;

    embi_css_slice cvs = embi_css_parse_cvs(&s->arena, text);
    embi_css_items items = embi_css_consume_rules(&s->arena, cvs, true);
    rule_vec rv = {0};
    build_rules_into(&b, items, CTX_TOP, &rv);
    finish_rules(&s->arena, &rv, &s->rules, &s->nrules);
    embi_sb_free(&b.scratch);
    return s;
}

static embi_css_sheet *sheet_new(void)
{
    embi_css_sheet *s = embi_calloc(1, sizeof *s);
    atomic_store_explicit(&s->refs, 1, memory_order_relaxed);
    embi_arena_init(&s->arena, 16384);
    return s;
}

embi_css_sheet *embi_css_sheet_parse(embi_str text, const embi_css_parse_opts *opts)
{
    const embi_css_parse_opts *o = opts ? opts : &no_opts;
    import_state st = {0};
    if (o->base_url.len != 0) {
        embi_vec_push(&st.active, o->base_url);
    }
    embi_css_sheet *s = sheet_new();
    sheet_fill_text(s, text, o, &st, o->base_url);
    import_state_free(&st);
    return s;
}

embi_css_sheet *embi_css_sheet_parse_bytes(const unsigned char *p, size_t n, embi_str protocol,
                                           embi_str env, const embi_css_parse_opts *opts,
                                           const char **encoding)
{
    const embi_css_parse_opts *o = opts ? opts : &no_opts;
    embi_css_sheet *s = sheet_new();
    embi_str text;
    if (!embi_css_decode_bytes(&s->arena, p, n, protocol, env, &text, encoding)) {
        embi_css_sheet_unref(s);
        return nullptr;
    }
    import_state st = {0};
    if (o->base_url.len != 0) {
        embi_vec_push(&st.active, o->base_url);
    }
    sheet_fill_text(s, text, o, &st, o->base_url);
    import_state_free(&st);
    return s;
}

void embi_css_sheet_ref(embi_css_sheet *s)
{
    atomic_fetch_add_explicit(&s->refs, 1, memory_order_relaxed);
}

void embi_css_sheet_unref(embi_css_sheet *s)
{
    if (s == nullptr) {
        return;
    }
    /* The last release must see every write made before the other releases. */
    if (atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1) {
        return;
    }
    for (uint32_t k = 0; k < s->nrules; k++) {
        if (s->rules[k].kind == EMBI_CSS_RULE_IMPORT && s->rules[k].sheet != nullptr) {
            embi_css_sheet_unref(s->rules[k].sheet);
        }
    }
    embi_arena_free(&s->arena);
    embi_free(s, sizeof *s);
}
