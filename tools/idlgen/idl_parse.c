/*
 * idlgen: the WebIDL tokenizer, the parser and the resolver (see idl.h for the supported subset).
 *
 * Tokens are collected for a whole file first, so the parser can look ahead freely. The first
 * syntax error ends the parse of the file (the generator would have nothing sound to emit after it).
 */
#include "idl.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ allocation helpers */

/* Every block the parser makes is registered here, so idl_db_free releases all of them. The tool
 * parses one database at a time. */
static void **g_blocks;
static int g_nblocks, g_capblocks;

static void register_block(void *p)
{
    if (g_nblocks == g_capblocks) {
        int nc = g_capblocks ? g_capblocks * 2 : 256;
        void **nb = realloc(g_blocks, (size_t)nc * sizeof *nb);
        if (!nb) {
            fprintf(stderr, "idlgen: out of memory\n");
            exit(2);
        }
        g_blocks = nb;
        g_capblocks = nc;
    }
    g_blocks[g_nblocks++] = p;
}

static void *idl_xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "idlgen: out of memory\n");
        exit(2);
    }
    register_block(p);
    return p;
}

void idl_list_reserve(void **items, int *cap, int n, size_t elem_size)
{
    if (n < *cap) {
        return;
    }
    int nc = *cap ? *cap * 2 : 8;
    void *old = *items;
    void *np = realloc(old, (size_t)nc * elem_size);
    if (!np) {
        fprintf(stderr, "idlgen: out of memory\n");
        exit(2);
    }
    bool replaced = false;
    if (old) {
        for (int i = 0; i < g_nblocks && !replaced; i++) {
            if (g_blocks[i] == old) {
                g_blocks[i] = np;
                replaced = true;
            }
        }
    }
    if (!replaced) {
        register_block(np);
    }
    *items = np;
    *cap = nc;
}

void idl_db_free(idl_db *db)
{
    for (int i = 0; i < g_nblocks; i++) {
        free(g_blocks[i]);
    }
    free(g_blocks);
    g_blocks = nullptr;
    g_nblocks = g_capblocks = 0;
    *db = (idl_db){0};
}

static const char *idl_strndup(const char *s, size_t n)
{
    char *d = idl_xalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static idl_type *idl_new_type(uint8_t kind, int line)
{
    idl_type *t = idl_xalloc(sizeof *t);
    t->kind = kind;
    t->line = line;
    return t;
}

static idl_type *idl_new_sub(uint8_t kind, int line, int nsub)
{
    idl_type *t = idl_new_type(kind, line);
    t->sub = idl_xalloc(sizeof *t->sub * (size_t)nsub);
    t->nsub = nsub;
    return t;
}

static void idl_ext_push(idl_exts *e, const char *name, const char *value)
{
    idl_list_reserve((void **)&e->items, &e->cap, e->n, sizeof *e->items);
    e->items[e->n].name = name;
    e->items[e->n].value = value;
    e->n++;
}

const idl_ext *idl_ext_get(const idl_exts *e, const char *name)
{
    for (int i = 0; i < e->n; i++) {
        if (strcmp(e->items[i].name, name) == 0) {
            return &e->items[i];
        }
    }
    return nullptr;
}

bool idl_has_ext(const idl_exts *e, const char *name)
{
    return idl_ext_get(e, name) != nullptr;
}

/* ------------------------------------------------------------------ tokenizer */

typedef enum tk_kind {
    TK_EOF,
    TK_ID,
    TK_INT,
    TK_FLOAT,
    TK_STR,
    TK_PUNCT,
    TK_ELLIPSIS,
} tk_kind;

typedef struct tok {
    uint8_t kind;
    char c;             /* TK_PUNCT */
    const char *s;      /* text: identifier, number spelling, string content */
    size_t n;
    int line;
} tok;

typedef struct parser {
    idl_db *db;
    const char *file;
    tok *toks;
    int ntoks, pos;
    int errors;
} parser;

void idl_report(idl_db *db, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

void idl_report(idl_db *db, const char *file, int line, const char *fmt, ...)
{
    char buf[IDL_MAX_ERR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char full[IDL_MAX_ERR + 128];
    snprintf(full, sizeof full, "%s.webidl:%d: %s", file ? file : "?", line, buf);
    if (db->nerrors == 0) {
        snprintf(db->first_error, sizeof db->first_error, "%.*s", (int)sizeof db->first_error - 1, full);
    }
    db->nerrors++;
    fprintf(stderr, "idlgen: %s\n", full);
}

static bool id_start(int c)
{
    return isalpha(c) || c == '_';
}

static bool id_part(int c)
{
    return isalnum(c) || c == '_';
}

/* Returns the number of tokens, or -1 on a lexical error (already reported). */
static int tokenize(idl_db *db, const char *file, const char *text, size_t len, tok **out)
{
    int cap = 256, n = 0, line = 1;
    tok *v = calloc((size_t)cap, sizeof *v); // owned by the caller (idl_parse), not the block list
    if (!v) {
        exit(2);
    }
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            line++;
            i++;
            continue;
        }
        if (isspace(c)) {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '/') {
            while (i < len && text[i] != '\n') {
                i++;
            }
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i + 1] == '/')) {
                if (text[i] == '\n') {
                    line++;
                }
                i++;
            }
            if (i + 1 >= len) {
                idl_report(db, file, line, "unterminated comment");
                free(v);
                return -1;
            }
            i += 2;
            continue;
        }
        tok t = {0};
        t.line = line;
        if (n == cap) {
            cap *= 2;
            v = realloc(v, sizeof *v * (size_t)cap);
            if (!v) {
                exit(2);
            }
        }
        if (id_start(c)) {
            size_t b = i;
            while (i < len && id_part((unsigned char)text[i])) {
                i++;
            }
            t.kind = TK_ID;
            t.s = text + b;
            t.n = i - b;
        } else if (isdigit(c) || (c == '.' && i + 1 < len && isdigit((unsigned char)text[i + 1]))) {
            size_t b = i;
            bool flt = false;
            if (c == '0' && i + 1 < len && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                i += 2;
                while (i < len && isxdigit((unsigned char)text[i])) {
                    i++;
                }
            } else {
                while (i < len && isdigit((unsigned char)text[i])) {
                    i++;
                }
                if (i < len && text[i] == '.') {
                    flt = true;
                    i++;
                    while (i < len && isdigit((unsigned char)text[i])) {
                        i++;
                    }
                }
                if (i < len && (text[i] == 'e' || text[i] == 'E')) {
                    flt = true;
                    i++;
                    if (i < len && (text[i] == '+' || text[i] == '-')) {
                        i++;
                    }
                    while (i < len && isdigit((unsigned char)text[i])) {
                        i++;
                    }
                }
            }
            t.kind = flt ? TK_FLOAT : TK_INT;
            t.s = text + b;
            t.n = i - b;
        } else if (c == '"') {
            size_t b = ++i;
            while (i < len && text[i] != '"') {
                if (text[i] == '\n') {
                    idl_report(db, file, line, "unterminated string");
                    free(v);
                    return -1;
                }
                i++;
            }
            if (i >= len) {
                idl_report(db, file, line, "unterminated string");
                free(v);
                return -1;
            }
            t.kind = TK_STR;
            t.s = text + b;
            t.n = i - b;
            i++;
        } else if (c == '.' && i + 2 < len && text[i + 1] == '.' && text[i + 2] == '.') {
            t.kind = TK_ELLIPSIS;
            i += 3;
        } else if (strchr("{}[]()<>,;:=?-|.", c)) {
            t.kind = TK_PUNCT;
            t.c = (char)c;
            i++;
        } else {
            idl_report(db, file, line, "unexpected character '%c'", isprint(c) ? c : '?');
            free(v);
            return -1;
        }
        v[n++] = t;
    }
    tok eof = {0};
    eof.kind = TK_EOF;
    eof.line = line;
    if (n == cap) {
        v = realloc(v, sizeof *v * (size_t)(cap + 1));
    }
    v[n++] = eof;
    *out = v;
    return n;
}

/* ------------------------------------------------------------------ parser helpers */

static tok *cur(parser *p)
{
    return &p->toks[p->pos];
}

static tok *peek_at(parser *p, int k)
{
    int i = p->pos + k;
    if (i >= p->ntoks) {
        i = p->ntoks - 1;
    }
    return &p->toks[i];
}

static void adv(parser *p)
{
    if (p->pos < p->ntoks - 1) {
        p->pos++;
    }
}

static bool is_p(parser *p, char c)
{
    return cur(p)->kind == TK_PUNCT && cur(p)->c == c;
}

static bool is_kw(parser *p, const char *kw)
{
    tok *t = cur(p);
    return t->kind == TK_ID && t->n == strlen(kw) && memcmp(t->s, kw, t->n) == 0;
}

static bool tok_is_kw(const tok *t, const char *kw)
{
    return t->kind == TK_ID && t->n == strlen(kw) && memcmp(t->s, kw, t->n) == 0;
}

static bool accept_p(parser *p, char c)
{
    if (is_p(p, c)) {
        adv(p);
        return true;
    }
    return false;
}

static bool accept_kw(parser *p, const char *kw)
{
    if (is_kw(p, kw)) {
        adv(p);
        return true;
    }
    return false;
}

/* Errors abort the file: the parser checks ok after each step. */
static void perr(parser *p, const char *what)
{
    tok *t = cur(p);
    char got[96];
    if (t->kind == TK_EOF) {
        snprintf(got, sizeof got, "end of file");
    } else if (t->kind == TK_PUNCT) {
        snprintf(got, sizeof got, "'%c'", t->c);
    } else {
        snprintf(got, sizeof got, "'%.*s'", (int)(t->n > 40 ? 40 : t->n), t->s);
    }
    idl_report(p->db, p->file, t->line, "expected %s, found %s", what, got);
    p->errors++;
    p->pos = p->ntoks - 1;
}

static bool expect_p(parser *p, char c)
{
    if (accept_p(p, c)) {
        return true;
    }
    char what[8];
    snprintf(what, sizeof what, "'%c'", c);
    perr(p, what);
    return false;
}

static const char *expect_id(parser *p, const char *what)
{
    tok *t = cur(p);
    if (t->kind != TK_ID) {
        perr(p, what);
        return nullptr;
    }
    adv(p);
    return idl_strndup(t->s, t->n);
}

/* ------------------------------------------------------------------ extended attributes */

static void skip_parens(parser *p)
{
    int depth = 0;
    do {
        if (is_p(p, '(')) {
            depth++;
        } else if (is_p(p, ')')) {
            depth--;
        } else if (cur(p)->kind == TK_EOF) {
            perr(p, "')'");
            return;
        }
        adv(p);
    } while (depth > 0 && p->errors == 0);
}

static void parse_exts(parser *p, idl_exts *out)
{
    if (!is_p(p, '[')) {
        return;
    }
    adv(p);
    while (p->errors == 0 && !is_p(p, ']')) {
        tok *t = cur(p);
        if (t->kind != TK_ID) {
            perr(p, "an extended attribute name");
            return;
        }
        const char *name = idl_strndup(t->s, t->n);
        adv(p);
        const char *value = nullptr;
        if (accept_p(p, '=')) {
            tok *v = cur(p);
            if (v->kind == TK_ID || v->kind == TK_INT || v->kind == TK_STR) {
                value = idl_strndup(v->s, v->n);
                adv(p);
            } else {
                perr(p, "an extended attribute value");
                return;
            }
        }
        if (is_p(p, '(')) {
            skip_parens(p);
        }
        idl_ext_push(out, name, value);
        if (!accept_p(p, ',')) {
            break;
        }
    }
    expect_p(p, ']');
}

/* ------------------------------------------------------------------ types */

static idl_type *parse_type(parser *p);

static idl_type *parse_type_core(parser *p)
{
    tok *t = cur(p);
    int line = t->line;
    if (t->kind == TK_PUNCT && t->c == '(') {
        adv(p);
        idl_type *u = idl_new_type(IDL_T_UNION, line);
        IDL_LIST(idl_type *) mem = {0};
        do {
            idl_type *m = parse_type(p);
            if (!m || p->errors) {
                return nullptr;
            }
            IDL_PUSH(p->db, mem, m);
        } while (accept_kw(p, "or") && p->errors == 0);
        expect_p(p, ')');
        if (p->errors) {
            return nullptr;
        }
        u->sub = mem.items;
        u->nsub = mem.n;
        return u;
    }
    if (t->kind != TK_ID) {
        perr(p, "a type");
        return nullptr;
    }
    if (tok_is_kw(t, "sequence") || tok_is_kw(t, "Promise") || tok_is_kw(t, "record")) {
        bool rec = tok_is_kw(t, "record");
        bool prom = tok_is_kw(t, "Promise");
        adv(p);
        if (!expect_p(p, '<')) {
            return nullptr;
        }
        idl_type *k = nullptr;
        if (rec) {
            k = parse_type(p);
            if (!k || !expect_p(p, ',')) {
                return nullptr;
            }
        }
        idl_type *v = parse_type(p);
        if (!v || !expect_p(p, '>')) {
            return nullptr;
        }
        if (rec) {
            idl_type *r = idl_new_sub(IDL_T_RECORD, line, 2);
            r->sub[0] = k;
            r->sub[1] = v;
            return r;
        }
        idl_type *r = idl_new_sub(prom ? IDL_T_PROMISE : IDL_T_SEQUENCE, line, 1);
        r->sub[0] = v;
        return r;
    }
    struct {
        const char *word;
        uint8_t kind;
    } simple[] = {
        {"any", IDL_T_ANY},          {"object", IDL_T_OBJECT},
        {"boolean", IDL_T_BOOLEAN},  {"octet", IDL_T_OCTET},
        {"byte", IDL_T_BYTE},        {"DOMString", IDL_T_DOMSTRING},
        {"USVString", IDL_T_USVSTRING}, {"ByteString", IDL_T_BYTESTRING},
        {"void", IDL_T_VOID},        {"undefined", IDL_T_VOID},
    };
    for (size_t i = 0; i < sizeof simple / sizeof simple[0]; i++) {
        if (tok_is_kw(t, simple[i].word)) {
            adv(p);
            return idl_new_type(simple[i].kind, line);
        }
    }
    if (tok_is_kw(t, "unsigned")) {
        adv(p);
        if (accept_kw(p, "short")) {
            return idl_new_type(IDL_T_USHORT, line);
        }
        if (!accept_kw(p, "long")) {
            perr(p, "'short' or 'long'");
            return nullptr;
        }
        if (accept_kw(p, "long")) {
            return idl_new_type(IDL_T_ULONGLONG, line);
        }
        return idl_new_type(IDL_T_ULONG, line);
    }
    if (tok_is_kw(t, "short")) {
        adv(p);
        return idl_new_type(IDL_T_SHORT, line);
    }
    if (tok_is_kw(t, "long")) {
        adv(p);
        if (accept_kw(p, "long")) {
            return idl_new_type(IDL_T_LONGLONG, line);
        }
        return idl_new_type(IDL_T_LONG, line);
    }
    if (tok_is_kw(t, "unrestricted")) {
        adv(p);
        bool dbl = accept_kw(p, "double");
        if (!dbl && !accept_kw(p, "float")) {
            perr(p, "'float' or 'double'");
            return nullptr;
        }
        idl_type *r = idl_new_type(dbl ? IDL_T_DOUBLE : IDL_T_FLOAT, line);
        r->unrestricted = true;
        return r;
    }
    if (tok_is_kw(t, "float")) {
        adv(p);
        return idl_new_type(IDL_T_FLOAT, line);
    }
    if (tok_is_kw(t, "double")) {
        adv(p);
        return idl_new_type(IDL_T_DOUBLE, line);
    }
    idl_type *n = idl_new_type(IDL_T_NAMED, line);
    n->name = idl_strndup(t->s, t->n);
    adv(p);
    return n;
}

static idl_type *parse_type(parser *p)
{
    idl_type *t = parse_type_core(p);
    if (!t || p->errors) {
        return nullptr;
    }
    if (accept_p(p, '?')) {
        t->nullable = true;
    }
    return t;
}

/* ------------------------------------------------------------------ default values */

/* A literal as text. Strings keep their quotes, so the generator can tell them from identifiers. */
static const char *parse_default(parser *p)
{
    tok *t = cur(p);
    if (t->kind == TK_STR) {
        char *s = idl_xalloc(t->n + 3);
        s[0] = '"';
        memcpy(s + 1, t->s, t->n);
        s[t->n + 1] = '"';
        adv(p);
        return s;
    }
    if (t->kind == TK_INT || t->kind == TK_FLOAT || t->kind == TK_ID) {
        const char *s = idl_strndup(t->s, t->n);
        adv(p);
        return s;
    }
    if (t->kind == TK_PUNCT && t->c == '-') {
        adv(p);
        tok *v = cur(p);
        if (v->kind == TK_INT || v->kind == TK_FLOAT || v->kind == TK_ID) {
            char *s = idl_xalloc(v->n + 2);
            s[0] = '-';
            memcpy(s + 1, v->s, v->n);
            adv(p);
            return s;
        }
        perr(p, "a number");
        return nullptr;
    }
    if (t->kind == TK_PUNCT && t->c == '[') {
        adv(p);
        expect_p(p, ']');
        return "[]";
    }
    if (t->kind == TK_PUNCT && t->c == '{') {
        adv(p);
        expect_p(p, '}');
        return "{}";
    }
    perr(p, "a default value");
    return nullptr;
}

/* ------------------------------------------------------------------ arguments and members */

static void parse_args(parser *p, idl_arg_list *out)
{
    if (!expect_p(p, '(')) {
        return;
    }
    if (accept_p(p, ')')) {
        return;
    }
    do {
        idl_arg a = {0};
        a.line = cur(p)->line;
        parse_exts(p, &a.ext);
        if (p->errors) {
            return;
        }
        a.optional = accept_kw(p, "optional");
        a.type = parse_type(p);
        if (!a.type || p->errors) {
            return;
        }
        if (accept_p(p, '.')) {
            /* "..." is lexed as one ellipsis token, so a lone '.' is an error. */
            perr(p, "'...'");
            return;
        }
        if (cur(p)->kind == TK_ELLIPSIS) {
            adv(p);
            a.variadic = true;
        }
        a.name = expect_id(p, "an argument name");
        if (!a.name) {
            return;
        }
        if (accept_p(p, '=')) {
            a.def = parse_default(p);
            if (p->errors) {
                return;
            }
        }
        IDL_PUSH(p->db, *out, a);
    } while (accept_p(p, ',') && p->errors == 0);
    expect_p(p, ')');
}

static void member_ext_flags(idl_member *m)
{
    m->throws = idl_has_ext(&m->ext, "Throws");
    m->sameobject = idl_has_ext(&m->ext, "SameObject");
    m->unforgeable = idl_has_ext(&m->ext, "LegacyUnforgeable");
    m->replaceable = idl_has_ext(&m->ext, "Replaceable");
    const idl_ext *pf = idl_ext_get(&m->ext, "PutForwards");
    if (pf) {
        m->put_forwards = pf->value;
    }
    const idl_ext *r = idl_ext_get(&m->ext, "Reflect");
    if (r) {
        m->reflect = true;
        m->reflect_name = r->value;
    }
}

/* Parses one member into *out. Returns false on a syntax error (already reported). */
static bool parse_member(parser *p, idl_iface *owner, idl_member *out)
{
    idl_member m = {0};
    m.owner = owner;
    m.line = cur(p)->line;
    parse_exts(p, &m.ext);
    if (p->errors) {
        return false;
    }
    if (is_kw(p, "constructor")) {
        adv(p);
        m.kind = IDL_M_CTOR;
        parse_args(p, &m.args);
    } else if (is_kw(p, "const")) {
        adv(p);
        m.kind = IDL_M_CONST;
        m.type = parse_type(p);
        if (!m.type) {
            return false;
        }
        m.name = expect_id(p, "a constant name");
        if (!m.name || !expect_p(p, '=')) {
            return false;
        }
        m.const_text = parse_default(p);
    } else if (is_kw(p, "iterable")) {
        adv(p);
        m.kind = IDL_M_ITERABLE;
        if (!expect_p(p, '<')) {
            return false;
        }
        idl_type *a = parse_type(p);
        if (!a) {
            return false;
        }
        if (accept_p(p, ',')) {
            m.key = a;
            m.type = parse_type(p);
            if (!m.type) {
                return false;
            }
        } else {
            m.type = a;
        }
        if (!expect_p(p, '>')) {
            return false;
        }
    } else if (is_kw(p, "maplike") || is_kw(p, "setlike")) {
        perr(p, "iterable (maplike and setlike are not supported)");
        return false;
    } else if (is_kw(p, "stringifier")) {
        adv(p);
        if (accept_p(p, ';')) {
            m.kind = IDL_M_STRINGIFIER;
            member_ext_flags(&m);
            *out = m;
            return true;
        } else {
            idl_member inner;
            if (!parse_member(p, owner, &inner)) {
                return false;
            }
            inner.stringifier = true;
            inner.ext = inner.ext;
            *out = inner;
            return true;
        }
    } else {
        if (accept_kw(p, "static")) {
            m.is_static = true;
        }
        if (accept_kw(p, "readonly")) {
            m.readonly = true;
        }
        if (accept_kw(p, "attribute")) {
            m.kind = IDL_M_ATTR;
            m.type = parse_type(p);
            if (!m.type) {
                return false;
            }
            m.name = expect_id(p, "an attribute name");
            if (!m.name) {
                return false;
            }
        } else {
            if (accept_kw(p, "getter")) {
                m.getter = true;
            }
            m.kind = IDL_M_OP;
            m.type = parse_type(p);
            if (!m.type) {
                return false;
            }
            if (!is_p(p, '(')) {
                m.name = expect_id(p, "an operation name");
                if (!m.name) {
                    return false;
                }
            }
            parse_args(p, &m.args);
        }
    }
    if (p->errors || !expect_p(p, ';')) {
        return false;
    }
    member_ext_flags(&m);
    *out = m;
    return true;
}

/* Parses the members of a body up to and including the closing brace. */
static void parse_members(parser *p, idl_iface *owner)
{
    while (p->errors == 0 && !is_p(p, '}')) {
        if (cur(p)->kind == TK_EOF) {
            perr(p, "'}'");
            return;
        }
        idl_member m;
        if (!parse_member(p, owner, &m)) {
            return;
        }
        IDL_PUSH(p->db, owner->members, m);
    }
    expect_p(p, '}');
}

static void parse_dict_members(parser *p, idl_dict *d)
{
    while (p->errors == 0 && !is_p(p, '}')) {
        if (cur(p)->kind == TK_EOF) {
            perr(p, "'}'");
            return;
        }
        idl_exts ext = {0};
        parse_exts(p, &ext);
        idl_dmember dm = {0};
        dm.line = cur(p)->line;
        dm.required = accept_kw(p, "required");
        dm.type = parse_type(p);
        if (!dm.type || p->errors) {
            return;
        }
        dm.name = expect_id(p, "a dictionary member name");
        if (!dm.name) {
            return;
        }
        if (accept_p(p, '=')) {
            dm.def = parse_default(p);
            if (p->errors) {
                return;
            }
        }
        if (!expect_p(p, ';')) {
            return;
        }
        IDL_PUSH(p->db, d->members, dm);
    }
    expect_p(p, '}');
}

/* ------------------------------------------------------------------ definitions */

static idl_iface *new_iface(const char *file, int line, const char *name)
{
    idl_iface *i = idl_xalloc(sizeof *i);
    i->name = name;
    i->file = file;
    i->line = line;
    return i;
}

static void parse_interface(parser *p, idl_exts *ext, bool partial, bool mixin, int line)
{
    const char *name = expect_id(p, "an interface name");
    if (!name) {
        return;
    }
    idl_iface *i = new_iface(p->file, line, name);
    i->mixin = mixin;
    i->is_partial = partial || mixin;
    i->ext = *ext;
    i->has_main = !partial && !mixin;
    if (accept_p(p, ':')) {
        if (partial || mixin) {
            perr(p, "'{'");
            return;
        }
        i->parent = expect_id(p, "a parent interface name");
        if (!i->parent) {
            return;
        }
    }
    if (!expect_p(p, '{')) {
        return;
    }
    parse_members(p, i);
    if (p->errors || !expect_p(p, ';')) {
        return;
    }
    IDL_PUSH(p->db, p->db->ifaces, i);
}

static void parse_definition(parser *p)
{
    idl_exts ext = {0};
    int line = cur(p)->line;
    parse_exts(p, &ext);
    if (p->errors) {
        return;
    }
    bool partial = accept_kw(p, "partial");
    if (is_kw(p, "interface")) {
        adv(p);
        bool mixin = accept_kw(p, "mixin");
        if (is_kw(p, "callback")) {
            /* callback interface X { ... }: keep it as an interface with the callback flag. */
            adv(p);
            if (!accept_kw(p, "interface")) {
                perr(p, "'interface'");
                return;
            }
            const char *name = expect_id(p, "a callback interface name");
            if (!name || !expect_p(p, '{')) {
                return;
            }
            idl_iface *i = new_iface(p->file, line, name);
            i->callback_iface = true;
            i->has_main = true;
            i->ext = ext;
            parse_members(p, i);
            if (p->errors || !expect_p(p, ';')) {
                return;
            }
            IDL_PUSH(p->db, p->db->ifaces, i);
            return;
        }
        parse_interface(p, &ext, partial, mixin, line);
        return;
    }
    if (is_kw(p, "namespace")) {
        adv(p);
        const char *name = expect_id(p, "a namespace name");
        if (!name || !expect_p(p, '{')) {
            return;
        }
        idl_iface *i = new_iface(p->file, line, name);
        i->ns = true;
        i->is_partial = partial;
        i->has_main = !partial;
        i->ext = ext;
        parse_members(p, i);
        if (p->errors || !expect_p(p, ';')) {
            return;
        }
        IDL_PUSH(p->db, p->db->ifaces, i);
        return;
    }
    if (is_kw(p, "dictionary")) {
        adv(p);
        const char *name = expect_id(p, "a dictionary name");
        if (!name) {
            return;
        }
        idl_dict *d = idl_xalloc(sizeof *d);
        d->name = name;
        d->file = p->file;
        d->line = line;
        d->ext = ext;
        d->is_partial = partial;
        if (accept_p(p, ':')) {
            d->parent = expect_id(p, "a parent dictionary name");
            if (!d->parent) {
                return;
            }
        }
        if (!expect_p(p, '{')) {
            return;
        }
        parse_dict_members(p, d);
        if (p->errors || !expect_p(p, ';')) {
            return;
        }
        IDL_PUSH(p->db, p->db->dicts, d);
        return;
    }
    if (is_kw(p, "enum")) {
        adv(p);
        const char *name = expect_id(p, "an enum name");
        if (!name || !expect_p(p, '{')) {
            return;
        }
        idl_enum *e = idl_xalloc(sizeof *e);
        e->name = name;
        e->file = p->file;
        e->line = line;
        while (p->errors == 0 && cur(p)->kind == TK_STR) {
            IDL_PUSH(p->db, e->values, idl_strndup(cur(p)->s, cur(p)->n));
            adv(p);
            if (!accept_p(p, ',')) {
                break;
            }
        }
        if (e->values.n == 0 && p->errors == 0) {
            perr(p, "an enum value");
            return;
        }
        if (p->errors || !expect_p(p, '}') || !expect_p(p, ';')) {
            return;
        }
        IDL_PUSH(p->db, p->db->enums, e);
        return;
    }
    if (is_kw(p, "typedef")) {
        adv(p);
        idl_exts tx = {0};
        parse_exts(p, &tx);
        idl_typedef *td = idl_xalloc(sizeof *td);
        td->line = line;
        td->type = parse_type(p);
        if (!td->type) {
            return;
        }
        td->name = expect_id(p, "a typedef name");
        if (!td->name || !expect_p(p, ';')) {
            return;
        }
        IDL_PUSH(p->db, p->db->typedefs, td);
        return;
    }
    if (is_kw(p, "callback")) {
        adv(p);
        if (is_kw(p, "interface")) {
            adv(p);
            const char *iname = expect_id(p, "a callback interface name");
            if (!iname || !expect_p(p, '{')) {
                return;
            }
            idl_iface *ci = new_iface(p->file, line, iname);
            ci->callback_iface = true;
            ci->has_main = true;
            ci->ext = ext;
            parse_members(p, ci);
            if (p->errors || !expect_p(p, ';')) {
                return;
            }
            IDL_PUSH(p->db, p->db->ifaces, ci);
            return;
        }
        const char *name = expect_id(p, "a callback name");
        if (!name || !expect_p(p, '=')) {
            return;
        }
        idl_callback *cb = idl_xalloc(sizeof *cb);
        cb->name = name;
        cb->file = p->file;
        cb->line = line;
        cb->ret = parse_type(p);
        if (!cb->ret) {
            return;
        }
        parse_args(p, &cb->args);
        if (p->errors || !expect_p(p, ';')) {
            return;
        }
        IDL_PUSH(p->db, p->db->callbacks, cb);
        return;
    }
    if (cur(p)->kind == TK_ID && tok_is_kw(peek_at(p, 1), "includes")) {
        const char *target = expect_id(p, "an interface name");
        adv(p); /* includes */
        const char *mixin = expect_id(p, "a mixin name");
        if (!target || !mixin || !expect_p(p, ';')) {
            return;
        }
        /* Recorded as a separate statement: resolve adds the mixin's members to target. */
        idl_iface *i = new_iface(p->file, line, target);
        i->is_partial = true;
        IDL_PUSH(p->db, i->includes, mixin);
        i->ext = ext;
        i->has_main = false;
        /* A marker record: its name is the target, its only job is the include list. */
        i->mixin = false;
        IDL_PUSH(p->db, p->db->ifaces, i);
        return;
    }
    perr(p, "a definition");
}

/* ------------------------------------------------------------------ public parse entry */

int idl_parse(idl_db *db, const char *name, const char *text, size_t len)
{
    int before = db->nerrors;
    name = idl_strndup(name, strlen(name));
    IDL_PUSH(db, db->files, name);
    parser p = {0};
    p.db = db;
    p.file = name;
    int n = tokenize(db, name, text, len, &p.toks);
    if (n < 0) {
        return db->nerrors - before;
    }
    p.ntoks = n;
    while (cur(&p)->kind != TK_EOF && p.errors == 0) {
        parse_definition(&p);
    }
    free(p.toks);
    return db->nerrors - before;
}


/* ------------------------------------------------------------------ lookup */

idl_iface *idl_find_iface(const idl_db *db, const char *name)
{
    for (int i = 0; i < db->ifaces.n; i++) {
        idl_iface *r = db->ifaces.items[i];
        if (r->has_main && !r->mixin && strcmp(r->name, name) == 0) {
            return r;
        }
    }
    return nullptr;
}

idl_dict *idl_find_dict(const idl_db *db, const char *name)
{
    for (int i = 0; i < db->dicts.n; i++) {
        idl_dict *d = db->dicts.items[i];
        if (!d->is_partial && strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return nullptr;
}

idl_enum *idl_find_enum(const idl_db *db, const char *name)
{
    for (int i = 0; i < db->enums.n; i++) {
        if (strcmp(db->enums.items[i]->name, name) == 0) {
            return db->enums.items[i];
        }
    }
    return nullptr;
}

bool idl_is_node(const idl_iface *i)
{
    return i && i->node_derived;
}

/* ------------------------------------------------------------------ resolve */

static idl_typedef *find_typedef(const idl_db *db, const char *name)
{
    for (int i = 0; i < db->typedefs.n; i++) {
        if (strcmp(db->typedefs.items[i]->name, name) == 0) {
            return db->typedefs.items[i];
        }
    }
    return nullptr;
}

static idl_callback *find_callback(const idl_db *db, const char *name)
{
    for (int i = 0; i < db->callbacks.n; i++) {
        if (strcmp(db->callbacks.items[i]->name, name) == 0) {
            return db->callbacks.items[i];
        }
    }
    return nullptr;
}

/* Replaces a named type by what it names. Errors are reported with file and line. */
static void resolve_type(idl_db *db, const char *file, idl_type *t, int depth)
{
    if (!t || t->resolved) {
        return;
    }
    if (depth > 16) {
        idl_report(db, file, t->line, "typedef chain too deep");
        return;
    }
    if (t->kind == IDL_T_NAMED) {
        const char *name = t->name;
        idl_typedef *td = find_typedef(db, name);
        if (td) {
            resolve_type(db, file, td->type, depth + 1);
            idl_type c = *td->type;
            if (t->nullable) {
                c.nullable = true;
            }
            c.line = t->line;
            *t = c;
            return;
        }
        idl_iface *i = idl_find_iface(db, name);
        idl_dict *d = idl_find_dict(db, name);
        idl_enum *e = idl_find_enum(db, name);
        idl_callback *cb = find_callback(db, name);
        if (i) {
            t->kind = i->callback_iface ? IDL_T_CALLBACK : IDL_T_INTERFACE;
            t->cb_iface = i->callback_iface;
            t->target = i;
        } else if (d) {
            t->kind = IDL_T_DICT;
            t->target = d;
        } else if (e) {
            t->kind = IDL_T_ENUM;
            t->target = e;
        } else if (cb) {
            t->kind = IDL_T_CALLBACK;
            t->target = cb;
        } else {
            idl_report(db, file, t->line, "unknown type '%s'", name);
            return;
        }
    } else {
        for (int k = 0; k < t->nsub; k++) {
            resolve_type(db, file, t->sub[k], depth + 1);
        }
    }
    t->resolved = true;
}

static void resolve_args(idl_db *db, const char *file, idl_arg_list *args)
{
    for (int i = 0; i < args->n; i++) {
        resolve_type(db, file, args->items[i].type, 0);
    }
}

static void resolve_members(idl_db *db, idl_iface *r)
{
    const char *file = r->file;
    for (int i = 0; i < r->members.n; i++) {
        idl_member *m = &r->members.items[i];
        if (m->type) {
            resolve_type(db, file, m->type, 0);
        }
        if (m->key) {
            resolve_type(db, file, m->key, 0);
        }
        resolve_args(db, file, &m->args);
    }
}

/* Partial interfaces and include markers add their members and includes to the main record. */
static void merge_partials(idl_db *db)
{
    for (int i = 0; i < db->ifaces.n; i++) {
        idl_iface *r = db->ifaces.items[i];
        if (r->mixin || r->has_main) {
            continue;
        }
        idl_iface *main = idl_find_iface(db, r->name);
        if (!main) {
            idl_report(db, r->file, r->line, "partial interface '%s' has no definition", r->name);
            continue;
        }
        if (r->ns != main->ns) {
            idl_report(db, r->file, r->line, "partial '%s' does not match its definition", r->name);
            continue;
        }
        for (int k = 0; k < r->members.n; k++) {
            IDL_PUSH(db, main->members, r->members.items[k]);
        }
        for (int k = 0; k < r->includes.n; k++) {
            IDL_PUSH(db, main->includes, r->includes.items[k]);
        }
    }
}

/* Copies the members of every mixin a main interface includes into it. */
static void expand_includes(idl_db *db)
{
    for (int i = 0; i < db->ifaces.n; i++) {
        idl_iface *r = db->ifaces.items[i];
        if (!r->has_main || r->mixin) {
            continue;
        }
        for (int k = 0; k < r->includes.n; k++) {
            const char *mname = r->includes.items[k];
            bool found = false;
            for (int j = 0; j < db->ifaces.n; j++) {
                idl_iface *mx = db->ifaces.items[j];
                if (!mx->mixin || strcmp(mx->name, mname) != 0) {
                    continue;
                }
                found = true;
                for (int q = 0; q < mx->members.n; q++) {
                    idl_member copy = mx->members.items[q];
                    copy.owner = r;
                    IDL_PUSH(db, r->members, copy);
                }
            }
            if (!found) {
                idl_report(db, r->file, r->line, "unknown mixin '%s'", mname);
            }
        }
        r->includes.n = 0;
    }
}

static void resolve_parents(idl_db *db)
{
    for (int i = 0; i < db->ifaces.n; i++) {
        idl_iface *r = db->ifaces.items[i];
        if (!r->has_main || r->mixin || !r->parent) {
            continue;
        }
        idl_iface *p = idl_find_iface(db, r->parent);
        if (!p) {
            idl_report(db, r->file, r->line, "unknown parent interface '%s'", r->parent);
            continue;
        }
        r->parent_decl = p;
    }
    for (int i = 0; i < db->dicts.n; i++) {
        idl_dict *d = db->dicts.items[i];
        if (d->is_partial || !d->parent) {
            continue;
        }
        idl_dict *p = idl_find_dict(db, d->parent);
        if (!p) {
            idl_report(db, d->file, d->line, "unknown parent dictionary '%s'", d->parent);
            continue;
        }
        d->parent_decl = p;
    }
    for (int i = 0; i < db->ifaces.n; i++) {
        idl_iface *r = db->ifaces.items[i];
        if (!r->has_main || r->mixin) {
            continue;
        }
        bool node = strcmp(r->name, "Node") == 0;
        for (idl_iface *p = r->parent_decl; p && !node; p = p->parent_decl) {
            node = strcmp(p->name, "Node") == 0;
        }
        r->node_derived = node;
    }
}

static int cmp_iface_name(const void *a, const void *b)
{
    const idl_iface *x = *(idl_iface *const *)a;
    const idl_iface *y = *(idl_iface *const *)b;
    return strcmp(x->name, y->name);
}

int idl_type_cat(const idl_type *t, int *iface, bool *nullable)
{
    *iface = 0;
    *nullable = t->nullable;
    switch (t->kind) {
    case IDL_T_INTERFACE: {
        const idl_iface *i = t->target;
        *iface = i->id;
        return idl_is_node(i) ? 0 : 1;
    }
    case IDL_T_CALLBACK:
        return 2;
    case IDL_T_SEQUENCE:
        return 3;
    case IDL_T_DICT:
        *nullable = true;
        return 4;
    case IDL_T_RECORD:
    case IDL_T_OBJECT:
        return 4;
    case IDL_T_BOOLEAN:
        return 5;
    case IDL_T_FLOAT:
    case IDL_T_DOUBLE:
        return 6;
    case IDL_T_ENUM:
    case IDL_T_DOMSTRING:
    case IDL_T_USVSTRING:
    case IDL_T_BYTESTRING:
        return 7;
    case IDL_T_UNION:
    case IDL_T_ANY:
    case IDL_T_PROMISE:
        return 8;
    default:
        return (t->kind >= IDL_T_OCTET && t->kind <= IDL_T_ULONGLONG) ? 6 : 8;
    }
}

/* Two overloads that can be called with the same number of arguments must differ in the category
 * (or interface) of some argument. */
static void check_overloads(idl_db *db, idl_iface *r)
{
    for (int i = 0; i < r->members.n; i++) {
        const idl_member *a = &r->members.items[i];
        if (a->kind != IDL_M_OP || a->getter || !a->name || a->noverloads < 2) {
            continue;
        }
        for (int k = i + 1; k < r->members.n; k++) {
            const idl_member *b = &r->members.items[k];
            if (b->kind != IDL_M_OP || b->getter || !b->name || b->is_static != a->is_static ||
                strcmp(a->name, b->name) != 0) {
                continue;
            }
            int amin = 0, bmin = 0;
            bool aopt = false, bopt = false;
            bool avar = a->args.n && a->args.items[a->args.n - 1].variadic;
            bool bvar = b->args.n && b->args.items[b->args.n - 1].variadic;
            for (int q = 0; q < a->args.n; q++) {
                aopt = aopt || a->args.items[q].optional || a->args.items[q].variadic;
                if (!aopt) {
                    amin = q + 1;
                }
            }
            for (int q = 0; q < b->args.n; q++) {
                bopt = bopt || b->args.items[q].optional || b->args.items[q].variadic;
                if (!bopt) {
                    bmin = q + 1;
                }
            }
            int amax = avar ? 255 : a->args.n, bmax = bvar ? 255 : b->args.n;
            int lo = amin > bmin ? amin : bmin;
            int hi = amax < bmax ? amax : bmax;
            if (lo > hi) {
                continue; /* never callable with the same count */
            }
            bool distinct = false;
            int n = a->args.n < b->args.n ? a->args.n : b->args.n;
            for (int q = 0; q < n && !distinct; q++) {
                int ia, ib;
                bool na, nb;
                int ca = idl_type_cat(a->args.items[q].type, &ia, &na);
                int cb = idl_type_cat(b->args.items[q].type, &ib, &nb);
                if ((ca != cb || ((ca == 0 || ca == 1) && ia != ib)) && ca != 8 && cb != 8) {
                    distinct = true;
                }
            }
            if (!distinct) {
                idl_report(db, r->file, b->line,
                           "overloads of '%s' are not distinguishable by argument count or type",
                           a->name);
            }
        }
    }
}

/* Overloads of one name, counted in declaration order. */
static void number_overloads(idl_iface *r)
{
    for (int i = 0; i < r->members.n; i++) {
        idl_member *m = &r->members.items[i];
        if (m->kind != IDL_M_OP || m->getter || !m->name) {
            continue;
        }
        int before = 0, total = 0;
        for (int k = 0; k < r->members.n; k++) {
            const idl_member *o = &r->members.items[k];
            if (o->kind == IDL_M_OP && !o->getter && o->name && strcmp(o->name, m->name) == 0 &&
                o->is_static == m->is_static) {
                total++;
                if (k < i) {
                    before++;
                }
            }
        }
        m->overload = before;
        m->noverloads = total;
        m->static_clash = false;
        for (int k = 0; m->is_static && k < r->members.n; k++) {
            const idl_member *o = &r->members.items[k];
            if (o->kind == IDL_M_OP && o->name && !o->is_static && strcmp(o->name, m->name) == 0) {
                m->static_clash = true;
            }
        }
    }
}

int idl_resolve(idl_db *db)
{
    int before = db->nerrors;
    if (db->resolved) {
        return 0;
    }
    db->resolved = true;
    merge_partials(db);
    for (int i = 0; i < db->dicts.n; i++) {
        idl_dict *d = db->dicts.items[i];
        if (!d->is_partial) {
            continue;
        }
        idl_dict *main = idl_find_dict(db, d->name);
        if (!main) {
            idl_report(db, d->file, d->line, "partial dictionary '%s' has no definition", d->name);
            continue;
        }
        for (int k = 0; k < d->members.n; k++) {
            IDL_PUSH(db, main->members, d->members.items[k]);
        }
    }
    expand_includes(db);
    resolve_parents(db);
    for (int i = 0; i < db->ifaces.n; i++) {
        idl_iface *r = db->ifaces.items[i];
        if (r->has_main && !r->mixin) {
            resolve_members(db, r);
            number_overloads(r);
        }
    }
    for (int i = 0; i < db->dicts.n; i++) {
        idl_dict *d = db->dicts.items[i];
        if (d->is_partial) {
            continue;
        }
        for (int k = 0; k < d->members.n; k++) {
            resolve_type(db, d->file, d->members.items[k].type, 0);
        }
    }
    for (int i = 0; i < db->typedefs.n; i++) {
        resolve_type(db, "typedef", db->typedefs.items[i]->type, 0);
    }
    for (int i = 0; i < db->callbacks.n; i++) {
        idl_callback *cb = db->callbacks.items[i];
        resolve_type(db, cb->file, cb->ret, 0);
        resolve_args(db, cb->file, &cb->args);
    }
    /* Interface ids: sorted by name, for the main records that have a prototype object. */
    idl_iface **ids = idl_xalloc(sizeof *ids * (size_t)(db->ifaces.n + 1));
    int nids = 0;
    for (int i = 0; i < db->ifaces.n; i++) {
        idl_iface *r = db->ifaces.items[i];
        r->id = -1;
        if (r->has_main && !r->mixin && !r->ns && !r->callback_iface) {
            ids[nids++] = r;
        }
    }
    qsort(ids, (size_t)nids, sizeof *ids, cmp_iface_name);
    for (int i = 0; i < nids; i++) {
        ids[i]->id = i;
    }
    for (int i = 0; i < nids; i++) {
        check_overloads(db, ids[i]);
    }
    /* [SameObject] cache slots: numbered along the inheritance chain (a child continues after its
     * parent's slots), so interfaces of different chains may share slot numbers. */
    for (int round = 0; round < 64; round++) {
        for (int i = 0; i < nids; i++) {
            idl_iface *r = ids[i];
            int slot = r->parent_decl ? r->parent_decl->nslots : 0;
            for (int k = 0; k < r->members.n; k++) {
                idl_member *m = &r->members.items[k];
                if (m->kind == IDL_M_ATTR && m->sameobject) {
                    m->slot = slot++;
                }
            }
            r->nslots = slot;
        }
    }
    for (int i = 0; i < nids; i++) {
        if (ids[i]->nslots > IDL_MAX_SAME_SLOTS) {
            idl_report(db, ids[i]->file, ids[i]->line, "%s has %d [SameObject] slots, at most %d",
                       ids[i]->name, ids[i]->nslots, IDL_MAX_SAME_SLOTS);
        }
    }
    return db->nerrors - before;
}
