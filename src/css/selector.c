/*
 * Selectors Level 4 parsing (W2b): selector lists, compounds, attribute and pseudo-class syntax,
 * nth expressions (An+B), specificity, and the bloom requirements of each complex selector.
 *
 * Parsing runs over component values. Scratch arrays come from a temporary arena that is freed when
 * the parse returns; the compiled result is copied into the caller's arena.
 */
#include "css/selector.h"

#include <string.h>

typedef struct sp {
    embi_arena *out; /* the compiled result */
    embi_arena *tmp; /* scratch, freed at the end of each top-level parse */
    const embi_sel_env *env;
    bool in_args; /* inside a pseudo-class argument: pseudo-elements are invalid */
    bool in_has;  /* inside :has(): a nested :has() is invalid */
    uint32_t depth;    /* nesting depth of pseudo-class argument lists */
    bool too_deep;     /* the depth cap was hit: the whole parse is invalid */
} sp;

#define EMBI_SEL_MAX_DEPTH 32u

/* ------------------------------------------------------------------ small helpers */

static inline char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* ASCII case-insensitive comparison with a lowercase literal. */
static bool ieq_lit(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        if (lc(s.ptr[k]) != lit[k]) {
            return false;
        }
    }
    return true;
}

static bool starts_ci(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len < n) {
        return false;
    }
    for (size_t k = 0; k < n; k++) {
        if (lc(s.ptr[k]) != lit[k]) {
            return false;
        }
    }
    return true;
}

static inline bool is_delim(const embi_css_cv *c, uint32_t ch)
{
    return c->tok.type == EMBI_CSS_DELIM && c->tok.cp == ch;
}

static inline uint32_t skip_ws(const embi_css_cv *v, uint32_t n, uint32_t i)
{
    while (i < n && v[i].tok.type == EMBI_CSS_WS) {
        i++;
    }
    return i;
}

static inline bool is_comb_tok(const embi_css_cv *c)
{
    return is_delim(c, '>') || is_delim(c, '+') || is_delim(c, '~');
}

static inline uint8_t comb_of(const embi_css_cv *c)
{
    if (is_delim(c, '>')) {
        return EMBI_SC_CHILD;
    }
    if (is_delim(c, '+')) {
        return EMBI_SC_NEXT;
    }
    return EMBI_SC_SUBSEQ;
}

/* A small direct-mapped cache of atom to lowercase atom, per thread, so the hot paths (type
 * compares, bloom push and pop, index candidates) do not take the atom table's lock. Atom ids are
 * stable until embi_atom_shutdown, so the cache is dropped by embi_sel_lower_cache_reset, which
 * every entry point that starts a pass calls. */
enum { LOWER_CACHE = 256 };
static thread_local struct {
    embi_atom key[LOWER_CACHE];
    embi_atom val[LOWER_CACHE];
} g_lower;

void embi_sel_lower_cache_reset(void)
{
    memset(&g_lower, 0, sizeof g_lower);
}

static embi_atom lower_atom_slow(embi_atom a)
{
    embi_str s = embi_atom_str(a);
    bool upper = false;
    for (uint32_t k = 0; k < s.len; k++) {
        if (s.ptr[k] >= 'A' && s.ptr[k] <= 'Z') {
            upper = true;
            break;
        }
    }
    if (!upper) {
        return a;
    }
    char buf[256];
    char *p = buf;
    if (s.len > sizeof buf) {
        p = embi_alloc(s.len);
    }
    for (uint32_t k = 0; k < s.len; k++) {
        p[k] = lc(s.ptr[k]);
    }
    embi_atom r = embi_atom_intern(p, s.len);
    if (p != buf) {
        embi_free(p, s.len);
    }
    return r;
}

embi_atom embi_sel_lower_atom(embi_atom a)
{
    uint32_t slot = (uint32_t)(embi_hash64((uint64_t)a) & (LOWER_CACHE - 1));
    if (g_lower.key[slot] == a && a != 0) {
        return g_lower.val[slot];
    }
    embi_atom r = lower_atom_slow(a);
    g_lower.key[slot] = a;
    g_lower.val[slot] = r;
    return r;
}

static embi_atom intern_str(embi_str s)
{
    return embi_atom_intern(s.ptr, s.len);
}

static embi_str copy_str(embi_arena *a, embi_str s)
{
    char *p = embi_css_arena_copy(a, s.ptr, s.len, 1);
    return (embi_str){p, s.len};
}

int embi_sel_spec_cmp(embi_sel_spec x, embi_sel_spec y)
{
    if (x.a != y.a) {
        return x.a < y.a ? -1 : 1;
    }
    if (x.b != y.b) {
        return x.b < y.b ? -1 : 1;
    }
    if (x.c != y.c) {
        return x.c < y.c ? -1 : 1;
    }
    return 0;
}

static void spec_add(embi_sel_spec *x, embi_sel_spec y)
{
    x->a += y.a;
    x->b += y.b;
    x->c += y.c;
}

/* The most specific item of a list (zero for an empty list). */
static embi_sel_spec spec_max(const embi_sel_list *l)
{
    embi_sel_spec m = {0, 0, 0};
    if (l == nullptr) {
        return m;
    }
    for (uint32_t k = 0; k < l->n; k++) {
        if (embi_sel_spec_cmp(l->items[k].spec, m) > 0) {
            m = l->items[k].spec;
        }
    }
    return m;
}

/* ------------------------------------------------------------------ An+B */

static bool digits_value(const char *p, size_t n, int64_t *out)
{
    if (n == 0 || n > 15) {
        return false;
    }
    int64_t v = 0;
    for (size_t k = 0; k < n; k++) {
        if (p[k] < '0' || p[k] > '9') {
            return false;
        }
        v = v * 10 + (p[k] - '0');
    }
    *out = v;
    return true;
}

/* An unsigned integer token at v[i] (after optional whitespace), as the magnitude of a B. Writes the
 * next index. */
static bool anb_unsigned(const embi_css_cv *v, uint32_t n, uint32_t i, int64_t *mag, uint32_t *next)
{
    i = skip_ws(v, n, i);
    if (i >= n || v[i].tok.type != EMBI_CSS_NUMBER || !v[i].tok.flag) {
        return false;
    }
    const embi_str r = v[i].tok.repr;
    if (r.len > 0 && (r.ptr[0] == '+' || r.ptr[0] == '-')) {
        return false;
    }
    if (v[i].tok.num > 1e15) {
        return false;
    }
    *mag = (int64_t)v[i].tok.num;
    *next = i + 1;
    return true;
}

/* Parses what follows the 'n' of An+B: optional whitespace, then an optional B. B is a signed
 * integer token, or a '+' or '-' delimiter followed by an unsigned integer (whitespace allowed
 * between them). Only whitespace may follow. */
static bool anb_tail(const embi_css_cv *v, uint32_t n, uint32_t i, int64_t *b)
{
    i = skip_ws(v, n, i);
    if (i == n) {
        *b = 0;
        return true;
    }
    const embi_css_cv *c = &v[i];
    if (is_delim(c, '+') || is_delim(c, '-')) {
        int64_t mag = 0;
        uint32_t next = 0;
        if (!anb_unsigned(v, n, i + 1, &mag, &next)) {
            return false;
        }
        *b = is_delim(c, '-') ? -mag : mag;
        i = next;
    } else if (c->tok.type == EMBI_CSS_NUMBER && c->tok.flag && c->tok.repr.len > 0 &&
               (c->tok.repr.ptr[0] == '+' || c->tok.repr.ptr[0] == '-')) {
        if (c->tok.num > 1e15 || c->tok.num < -1e15) {
            return false;
        }
        *b = (int64_t)c->tok.num;
        i++;
    } else {
        return false;
    }
    return skip_ws(v, n, i) == n;
}

/* The unit or ident was "n-" (or "-n-"): the B is the negated unsigned integer that follows, with
 * whitespace allowed. */
static bool anb_dash_only(const embi_css_cv *v, uint32_t n, uint32_t i, int64_t *b, uint32_t *next)
{
    int64_t mag = 0;
    if (!anb_unsigned(v, n, i, &mag, next)) {
        return false;
    }
    *b = -mag;
    return true;
}

/* An ident or unit at v[i] that is a prefix (of length `from`) followed by '-': digits in the rest
 * give the negated B. An empty rest takes the B from the unsigned integer that follows. */
static bool anb_dashed(const embi_css_cv *v, uint32_t n, uint32_t i, embi_str s, size_t from,
                       int64_t *b, uint32_t *next)
{
    if (s.len == from) {
        return anb_dash_only(v, n, i + 1, b, next);
    }
    if (!digits_value(s.ptr + from, s.len - from, b)) {
        return false;
    }
    *b = -*b;
    *next = i + 1;
    return true;
}

bool embi_sel_parse_anb(embi_css_slice in, int64_t *out_a, int64_t *out_b)
{
    const embi_css_cv *v = in.p;
    uint32_t n = in.n;
    uint32_t i = skip_ws(v, n, 0);
    if (i >= n) {
        return false;
    }
    const embi_css_token *t = &v[i].tok;
    int64_t a = 0;
    int64_t b = 0;

    if (t->type == EMBI_CSS_IDENT) {
        if (ieq_lit(t->value, "odd")) {
            a = 2;
            b = 1;
            i++;
        } else if (ieq_lit(t->value, "even")) {
            a = 2;
            b = 0;
            i++;
        } else if (ieq_lit(t->value, "n")) {
            a = 1;
            if (!anb_tail(v, n, i + 1, &b)) {
                return false;
            }
            i = n;
        } else if (ieq_lit(t->value, "-n")) {
            a = -1;
            if (!anb_tail(v, n, i + 1, &b)) {
                return false;
            }
            i = n;
        } else if (starts_ci(t->value, "n-")) {
            a = 1;
            uint32_t nx = 0;
            if (!anb_dashed(v, n, i, t->value, 2, &b, &nx)) {
                return false;
            }
            i = nx;
        } else if (starts_ci(t->value, "-n-")) {
            a = -1;
            uint32_t nx = 0;
            if (!anb_dashed(v, n, i, t->value, 3, &b, &nx)) {
                return false;
            }
            i = nx;
        } else {
            return false;
        }
    } else if (t->type == EMBI_CSS_DIMENSION) {
        if (!t->flag) {
            return false;
        }
        if (t->num > 1e15 || t->num < -1e15) {
            return false;
        }
        a = (int64_t)t->num;
        if (ieq_lit(t->value, "n")) {
            if (!anb_tail(v, n, i + 1, &b)) {
                return false;
            }
            i = n;
        } else if (starts_ci(t->value, "n-")) {
            uint32_t nx = 0;
            if (!anb_dashed(v, n, i, t->value, 2, &b, &nx)) {
                return false;
            }
            i = nx;
        } else {
            return false;
        }
    } else if (t->type == EMBI_CSS_NUMBER) {
        if (!t->flag || t->num > 1e15 || t->num < -1e15) {
            return false;
        }
        b = (int64_t)t->num;
        i++;
    } else if (is_delim(&v[i], '+')) {
        i++;
        if (i >= n || v[i].tok.type != EMBI_CSS_IDENT) {
            return false;
        }
        const embi_str id = v[i].tok.value;
        if (ieq_lit(id, "n")) {
            a = 1;
            if (!anb_tail(v, n, i + 1, &b)) {
                return false;
            }
            i = n;
        } else if (starts_ci(id, "n-")) {
            a = 1;
            uint32_t nx = 0;
            if (!anb_dashed(v, n, i, id, 2, &b, &nx)) {
                return false;
            }
            i = nx;
        } else {
            return false;
        }
    } else {
        return false;
    }

    if (skip_ws(v, n, i) != n) {
        return false;
    }
    *out_a = a;
    *out_b = b;
    return true;
}

/* ------------------------------------------------------------------ attribute selectors */

/* HTML attribute names whose values compare ASCII case-insensitively on HTML elements. */
static bool html_ci_attr(embi_atom lname)
{
    static const char *const names[] = {
        "accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset", "checked",
        "clear", "codetype", "color", "compact", "declare", "defer", "dir", "direction", "disabled",
        "enctype", "face", "frame", "hreflang", "http-equiv", "lang", "language", "link", "media",
        "method", "multiple", "nohref", "noresize", "noshade", "nowrap", "readonly", "rel", "rev",
        "rules", "scope", "scrolling", "selected", "shape", "target", "text", "type", "valign",
        "valuetype", "vlink",
    };
    embi_str s = embi_atom_str(lname);
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        if (ieq_lit(s, names[k])) {
            return true;
        }
    }
    return false;
}

/* Parses [ns|]name at v[*i]. For type selectors, an unprefixed name takes the default namespace
 * when one is declared. For attribute names, it has no namespace. */
static bool parse_qname(sp *p, const embi_css_cv *v, uint32_t n, uint32_t *ip, bool is_attr,
                        uint8_t *nsk, embi_atom *uri, bool *star, embi_str *name)
{
    enum { PFX_NONE, PFX_NAMED, PFX_ANY, PFX_EMPTY } pfx = PFX_NONE;
    embi_str prefix = {nullptr, 0};
    uint32_t k = *ip;
    *star = false;
    *uri = 0;

    if (k < n && v[k].tok.type == EMBI_CSS_IDENT && k + 1 < n && is_delim(&v[k + 1], '|')) {
        pfx = PFX_NAMED;
        prefix = v[k].tok.value;
        k += 2;
    } else if (k < n && is_delim(&v[k], '*') && k + 1 < n && is_delim(&v[k + 1], '|')) {
        pfx = PFX_ANY;
        k += 2;
    } else if (k < n && is_delim(&v[k], '|')) {
        pfx = PFX_EMPTY;
        k++;
    }

    if (k < n && v[k].tok.type == EMBI_CSS_IDENT) {
        *name = v[k].tok.value;
        k++;
    } else if (k < n && is_delim(&v[k], '*') && !is_attr) {
        *star = true;
        *name = (embi_str){nullptr, 0};
        k++;
    } else {
        return false;
    }

    switch (pfx) {
    case PFX_NONE:
        if (is_attr) {
            *nsk = EMBI_NSK_NONE;
        } else if (p->env != nullptr && p->env->nns > 0) {
            *nsk = EMBI_NSK_ANY;
            for (uint32_t j = 0; j < p->env->nns; j++) {
                if (p->env->ns[j].prefix == 0) {
                    *nsk = EMBI_NSK_URI;
                    *uri = p->env->ns[j].uri;
                    break;
                }
            }
        } else {
            *nsk = EMBI_NSK_ANY;
        }
        break;
    case PFX_NAMED: {
        embi_atom pa = intern_str(prefix);
        bool found = false;
        if (p->env != nullptr) {
            for (uint32_t j = 0; j < p->env->nns; j++) {
                if (p->env->ns[j].prefix == pa && pa != 0) {
                    *uri = p->env->ns[j].uri;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            return false; /* undeclared prefix: the selector is invalid */
        }
        *nsk = EMBI_NSK_URI;
        break;
    }
    case PFX_ANY:
        *nsk = EMBI_NSK_ANY;
        break;
    case PFX_EMPTY:
        *nsk = EMBI_NSK_NONE;
        break;
    }
    *ip = k;
    return true;
}

static bool parse_attr(sp *p, const embi_css_cv *v, uint32_t n, embi_sel_simple *s)
{
    uint32_t i = skip_ws(v, n, 0);
    uint8_t nsk = EMBI_NSK_NONE;
    embi_atom uri = 0;
    bool star = false;
    embi_str name = {nullptr, 0};
    if (!parse_qname(p, v, n, &i, true, &nsk, &uri, &star, &name)) {
        return false;
    }
    s->kind = EMBI_SS_ATTR;
    s->nsk = nsk;
    s->ns_uri = uri;
    s->name = intern_str(name);
    s->lname = embi_sel_lower_atom(s->name);
    s->sub = EMBI_AO_EXISTS;
    s->flags = html_ci_attr(s->lname) ? EMBI_SAF_HTML_CI : 0;
    s->value = (embi_str){nullptr, 0};

    i = skip_ws(v, n, i);
    if (i == n) {
        return true;
    }
    if (is_delim(&v[i], '=')) {
        s->sub = EMBI_AO_EQ;
    } else if (v[i].tok.type == EMBI_CSS_INCLUDE_MATCH) {
        s->sub = EMBI_AO_WORD;
    } else if (v[i].tok.type == EMBI_CSS_DASH_MATCH) {
        s->sub = EMBI_AO_DASH;
    } else if (v[i].tok.type == EMBI_CSS_PREFIX_MATCH) {
        s->sub = EMBI_AO_PREFIX;
    } else if (v[i].tok.type == EMBI_CSS_SUFFIX_MATCH) {
        s->sub = EMBI_AO_SUFFIX;
    } else if (v[i].tok.type == EMBI_CSS_SUBSTRING_MATCH) {
        s->sub = EMBI_AO_SUBSTR;
    } else {
        return false;
    }
    i = skip_ws(v, n, i + 1);
    if (i >= n || (v[i].tok.type != EMBI_CSS_IDENT && v[i].tok.type != EMBI_CSS_STRING)) {
        return false;
    }
    s->value = copy_str(p->out, v[i].tok.value);
    i = skip_ws(v, n, i + 1);
    if (i < n && v[i].tok.type == EMBI_CSS_IDENT) {
        if (ieq_lit(v[i].tok.value, "i")) {
            s->flags = (uint8_t)((s->flags & ~EMBI_SAF_CS) | EMBI_SAF_CI);
        } else if (ieq_lit(v[i].tok.value, "s")) {
            s->flags = (uint8_t)((s->flags & ~EMBI_SAF_CI) | EMBI_SAF_CS);
        } else {
            return false;
        }
        i = skip_ws(v, n, i + 1);
    }
    return i == n;
}

/* ------------------------------------------------------------------ lists and complex selectors */

static const embi_sel_list *parse_list(sp *p, const embi_css_cv *v, uint32_t n, bool forgiving,
                                       bool relative);

/* Parses one complex selector from exactly v[0..n) (already trimmed). */
static bool parse_compound(sp *p, const embi_css_cv *v, uint32_t n, uint32_t *ip,
                           embi_sel_simple *ss, uint32_t *nss, uint8_t *pe);

static bool parse_pseudo_class_fn(sp *p, const embi_css_cv *c, embi_sel_simple *s);

/* The user-action pseudo-classes that may follow a pseudo-element (CSS Pseudo 4). */
static bool user_action_pc(embi_sel_pc pc)
{
    switch (pc) {
    case EMBI_PC_HOVER:
    case EMBI_PC_ACTIVE:
    case EMBI_PC_FOCUS:
    case EMBI_PC_FOCUS_VISIBLE:
    case EMBI_PC_FOCUS_WITHIN:
        return true;
    default:
        return false;
    }
}

static bool parse_colon(sp *p, const embi_css_cv *v, uint32_t n, uint32_t *ip,
                        embi_sel_simple *ss, uint32_t *nss, uint8_t *pe)
{
    uint32_t i = *ip;
    bool dbl = i + 1 < n && v[i + 1].tok.type == EMBI_CSS_COLON;
    uint32_t k = dbl ? i + 2 : i + 1;
    if (k >= n) {
        return false;
    }
    const embi_css_cv *c = &v[k];

    if (c->tok.type == EMBI_CSS_IDENT) {
        embi_str name = c->tok.value;
        static const struct {
            const char *n;
            embi_sel_pseudo pe;
            bool dbl_only;
        } pes[] = {
            {"before", EMBI_PE_BEFORE, false},
            {"after", EMBI_PE_AFTER, false},
            {"first-line", EMBI_PE_FIRST_LINE, false},
            {"first-letter", EMBI_PE_FIRST_LETTER, false},
            {"marker", EMBI_PE_MARKER, true},
            {"placeholder", EMBI_PE_PLACEHOLDER, true},
            {"selection", EMBI_PE_SELECTION, true},
            {"backdrop", EMBI_PE_BACKDROP, true},
            {"file-selector-button", EMBI_PE_FILE_SELECTOR_BUTTON, true},
            {"-webkit-slider-runnable-track", EMBI_PE_SLIDER_TRACK, true},
            {"-webkit-slider-thumb", EMBI_PE_SLIDER_THUMB, true},
            {"-webkit-scrollbar", EMBI_PE_SCROLLBAR, true},
            {"-webkit-scrollbar-thumb", EMBI_PE_SCROLLBAR_THUMB, true},
            {"-webkit-scrollbar-track", EMBI_PE_SCROLLBAR_TRACK, true},
            {"-webkit-scrollbar-corner", EMBI_PE_SCROLLBAR_CORNER, true},
        };
        for (size_t j = 0; j < sizeof pes / sizeof pes[0]; j++) {
            if (ieq_lit(name, pes[j].n)) {
                if (pes[j].dbl_only && !dbl) {
                    break;
                }
                if (p->in_args || *pe != EMBI_PE_NONE) {
                    return false;
                }
                *pe = (uint8_t)pes[j].pe;
                *ip = k + 1;
                return true;
            }
        }
        if (dbl) {
            return false;
        }
        static const struct {
            const char *n;
            embi_sel_pc pc;
        } pcs[] = {
            {"root", EMBI_PC_ROOT},
            {"empty", EMBI_PC_EMPTY},
            {"first-child", EMBI_PC_FIRST_CHILD},
            {"last-child", EMBI_PC_LAST_CHILD},
            {"only-child", EMBI_PC_ONLY_CHILD},
            {"first-of-type", EMBI_PC_FIRST_OF_TYPE},
            {"last-of-type", EMBI_PC_LAST_OF_TYPE},
            {"only-of-type", EMBI_PC_ONLY_OF_TYPE},
            {"scope", EMBI_PC_SCOPE},
            {"link", EMBI_PC_LINK},
            {"any-link", EMBI_PC_LINK},
            {"visited", EMBI_PC_VISITED},
            {"hover", EMBI_PC_HOVER},
            {"active", EMBI_PC_ACTIVE},
            {"focus", EMBI_PC_FOCUS},
            {"focus-visible", EMBI_PC_FOCUS_VISIBLE},
            {"focus-within", EMBI_PC_FOCUS_WITHIN},
            {"target", EMBI_PC_TARGET},
            {"modal", EMBI_PC_MODAL},
            {"popover-open", EMBI_PC_POPOVER_OPEN},
            {"open", EMBI_PC_OPEN},
            {"defined", EMBI_PC_DEFINED},
            {"enabled", EMBI_PC_ENABLED},
            {"disabled", EMBI_PC_DISABLED},
            {"checked", EMBI_PC_CHECKED},
            {"indeterminate", EMBI_PC_INDETERMINATE},
            {"required", EMBI_PC_REQUIRED},
            {"optional", EMBI_PC_OPTIONAL},
            {"read-only", EMBI_PC_READ_ONLY},
            {"read-write", EMBI_PC_READ_WRITE},
            {"placeholder-shown", EMBI_PC_PLACEHOLDER_SHOWN},
            {"default", EMBI_PC_DEFAULT},
            {"valid", EMBI_PC_VALID},
            {"invalid", EMBI_PC_INVALID},
            {"in-range", EMBI_PC_IN_RANGE},
            {"out-of-range", EMBI_PC_OUT_OF_RANGE},
        };
        for (size_t j = 0; j < sizeof pcs / sizeof pcs[0]; j++) {
            if (ieq_lit(name, pcs[j].n)) {
                if (*pe != EMBI_PE_NONE && !user_action_pc(pcs[j].pc)) {
                    return false; /* after a pseudo-element only user-action classes may follow */
                }
                embi_sel_simple s = {0};
                s.kind = EMBI_SS_PC;
                s.sub = (uint8_t)pcs[j].pc;
                ss[(*nss)++] = s;
                *ip = k + 1;
                return true;
            }
        }
        return false;
    }

    if (c->tok.type == EMBI_CSS_FUNCTION) {
        if (dbl || *pe != EMBI_PE_NONE) {
            return false;
        }
        embi_sel_simple s = {0};
        if (!parse_pseudo_class_fn(p, c, &s)) {
            return false;
        }
        ss[(*nss)++] = s;
        *ip = k + 1;
        return true;
    }
    return false;
}

static bool parse_compound(sp *p, const embi_css_cv *v, uint32_t n, uint32_t *ip,
                           embi_sel_simple *ss, uint32_t *nss, uint8_t *pe)
{
    uint32_t i = *ip;
    uint32_t start = *nss;
    embi_sel_simple dflt = {0};
    bool has_dflt = false;
    dflt.kind = EMBI_SS_UNIVERSAL;
    dflt.nsk = EMBI_NSK_ANY;
    if (p->env != nullptr) {
        for (uint32_t j = 0; j < p->env->nns; j++) {
            if (p->env->ns[j].prefix == 0) {
                dflt.nsk = EMBI_NSK_URI;
                dflt.ns_uri = p->env->ns[j].uri;
                has_dflt = true;
                break;
            }
        }
    }

    if (!(i < n && (v[i].tok.type == EMBI_CSS_IDENT || is_delim(&v[i], '*') ||
                    is_delim(&v[i], '|')))) {
        /* No type selector: an implicit universal selector in the default namespace, if any. */
        if (has_dflt) {
            ss[(*nss)++] = dflt;
        }
    } else {
        uint32_t k = i;
        uint8_t nsk = EMBI_NSK_ANY;
        embi_atom uri = 0;
        bool star = false;
        embi_str name = {nullptr, 0};
        if (!parse_qname(p, v, n, &k, false, &nsk, &uri, &star, &name)) {
            return false;
        }
        embi_sel_simple s = {0};
        s.nsk = nsk;
        s.ns_uri = uri;
        if (star) {
            s.kind = EMBI_SS_UNIVERSAL;
        } else {
            s.kind = EMBI_SS_TYPE;
            s.name = intern_str(name);
            s.lname = embi_sel_lower_atom(s.name);
        }
        ss[(*nss)++] = s;
        i = k;
    }

    for (;;) {
        if (i >= n) {
            break;
        }
        const embi_css_cv *c = &v[i];
        embi_sel_simple s = {0};
        if (*pe != EMBI_PE_NONE && c->tok.type != EMBI_CSS_COLON) {
            /* After a pseudo-element, only user-action pseudo-classes may follow in the same
             * compound. Anything else that would extend the compound is invalid. Whitespace and
             * combinators end the compound and are handled by the caller. */
            if (c->tok.type == EMBI_CSS_HASH || c->tok.type == EMBI_CSS_LBRACKET ||
                is_delim(c, '.') || is_delim(c, '&')) {
                return false;
            }
            break;
        }
        if (c->tok.type == EMBI_CSS_HASH) {
            if (!c->tok.flag) {
                return false;
            }
            s.kind = EMBI_SS_ID;
            s.name = intern_str(c->tok.value);
            ss[(*nss)++] = s;
            i++;
        } else if (is_delim(c, '.')) {
            if (i + 1 >= n || v[i + 1].tok.type != EMBI_CSS_IDENT) {
                return false;
            }
            s.kind = EMBI_SS_CLASS;
            s.name = intern_str(v[i + 1].tok.value);
            ss[(*nss)++] = s;
            i += 2;
        } else if (c->tok.type == EMBI_CSS_LBRACKET) {
            if (!parse_attr(p, c->kids, c->nkids, &s)) {
                return false;
            }
            ss[(*nss)++] = s;
            i++;
        } else if (c->tok.type == EMBI_CSS_COLON) {
            if (!parse_colon(p, v, n, &i, ss, nss, pe)) {
                return false;
            }
        } else if (is_delim(c, '&')) {
            if (p->env != nullptr && p->env->nest_parent != nullptr) {
                s.kind = EMBI_SS_IS;
                s.list = p->env->nest_parent;
            } else {
                s.kind = EMBI_SS_PC;
                s.sub = EMBI_PC_SCOPE;
            }
            ss[(*nss)++] = s;
            i++;
        } else {
            break;
        }
    }

    if (*nss == start) {
        if (*pe == EMBI_PE_NONE) {
            return false;
        }
        /* A compound that is only a pseudo-element has an implicit universal selector. */
        ss[(*nss)++] = dflt;
    } else if (has_dflt && *nss == start + 1 && ss[start].kind == EMBI_SS_UNIVERSAL &&
               ss[start].nsk == EMBI_NSK_URI && *pe == EMBI_PE_NONE && i == *ip) {
        return false; /* nothing was parsed */
    }
    *ip = i;
    return true;
}

/* Adds the specificity of one simple selector. */
static void simple_spec(const embi_sel_simple *s, embi_sel_spec *acc)
{
    switch (s->kind) {
    case EMBI_SS_TYPE:
        acc->c++;
        break;
    case EMBI_SS_UNIVERSAL:
    case EMBI_SS_WHERE:
        break;
    case EMBI_SS_ID:
        acc->a++;
        break;
    case EMBI_SS_CLASS:
    case EMBI_SS_ATTR:
    case EMBI_SS_PC:
    case EMBI_SS_LANG:
    case EMBI_SS_DIR:
        acc->b++;
        break;
    case EMBI_SS_NTH:
        acc->b++;
        spec_add(acc, spec_max(s->list));
        break;
    case EMBI_SS_IS:
    case EMBI_SS_NOT:
    case EMBI_SS_HAS:
        spec_add(acc, spec_max(s->list));
        break;
    default:
        break;
    }
}

#define EMBI_SEL_MAX_COMPOUNDS 64u

static bool parse_complex(sp *p,const embi_css_cv *v, uint32_t n, bool relative,
                          embi_sel_complex *out)
{
    embi_sel_simple *ss = embi_arena_alloc(p->tmp, (size_t)(n + 1) * sizeof *ss, 8);
    embi_sel_compound *comp = embi_arena_alloc(p->tmp, (size_t)(n + 1) * sizeof *comp, 8);
    uint8_t *comb = embi_arena_alloc(p->tmp, (size_t)(n + 1), 8);
    uint32_t *cstart = embi_arena_alloc(p->tmp, (size_t)(n + 1) * sizeof *cstart, 8);
    uint32_t nss = 0;
    uint32_t nc = 0;
    uint8_t pe = EMBI_PE_NONE;
    uint8_t lead = EMBI_SC_NONE;
    uint8_t pending = EMBI_SC_DESC;
    uint32_t i = skip_ws(v, n, 0);

    if (relative) {
        lead = EMBI_SC_DESC;
        if (i < n && is_comb_tok(&v[i])) {
            lead = comb_of(&v[i]);
            i = skip_ws(v, n, i + 1);
        }
    }

    for (;;) {
        if (nc >= EMBI_SEL_MAX_COMPOUNDS) {
            return false; /* bounds match recursion depth and keeps the failure memo complete */
        }
        comb[nc] = (nc == 0) ? lead : pending;
        cstart[nc] = nss;
        if (!parse_compound(p, v, n, &i, ss, &nss, &pe)) {
            return false;
        }
        nc++;

        uint32_t before = i;
        i = skip_ws(v, n, i);
        bool had_ws = i != before;
        if (i >= n) {
            break;
        }
        if (pe != EMBI_PE_NONE) {
            return false; /* a pseudo-element must end the selector */
        }
        if (is_comb_tok(&v[i])) {
            pending = comb_of(&v[i]);
            i = skip_ws(v, n, i + 1);
            if (i >= n) {
                return false;
            }
        } else if (had_ws) {
            pending = EMBI_SC_DESC;
        } else {
            return false;
        }
    }
    cstart[nc] = nss;
    for (uint32_t k = 0; k < nc; k++) {
        comp[k].ss = nullptr;
        comp[k].n = cstart[k + 1] - cstart[k];
    }

    /* Copy into the result arena and fix the compound pointers. */
    embi_sel_simple *ss_out = embi_css_arena_copy(p->out, ss, nss, sizeof *ss);
    embi_sel_compound *comp_out = embi_css_arena_copy(p->out, comp, nc, sizeof *comp);
    for (uint32_t k = 0; k < nc; k++) {
        comp_out[k].ss = ss_out + cstart[k];
    }
    uint8_t *comb_out = embi_css_arena_copy(p->out, comb, nc, 1);

    embi_sel_spec spec = {0, 0, 0};
    for (uint32_t k = 0; k < nss; k++) {
        simple_spec(&ss[k], &spec);
    }
    if (pe != EMBI_PE_NONE) {
        spec.c++;
    }

    /* Bloom requirements: the compounds that must be strict ancestors of the subject. These are the
     * compounds to the left of an unbroken run of descendant or child combinators ending at the
     * subject. */
    uint32_t *req = embi_arena_alloc(p->tmp, (size_t)(nss + 1) * sizeof *req, 4);
    uint32_t nreq = 0;
    for (uint32_t k = nc - 1; k >= 1 && (comb[k] == EMBI_SC_DESC || comb[k] == EMBI_SC_CHILD); k--) {
        const embi_sel_compound *anc = &comp_out[k - 1];
        for (uint32_t j = 0; j < anc->n; j++) {
            const embi_sel_simple *s = &anc->ss[j];
            if (s->kind == EMBI_SS_ID || s->kind == EMBI_SS_CLASS) {
                req[nreq++] = embi_bloom_hash_atom(s->name);
            } else if (s->kind == EMBI_SS_TYPE && s->name == s->lname) {
                req[nreq++] = embi_bloom_hash_atom(s->lname);
            }
        }
    }
    const uint32_t *req_out = nullptr;
    if (nreq > 0) {
        req_out = embi_css_arena_copy(p->out, req, nreq, sizeof *req);
    }

    out->spec = spec;
    out->nc = nc;
    out->comp = comp_out;
    out->comb = comb_out;
    out->pseudo = pe;
    out->nreq = nreq;
    out->req = req_out;
    return true;
}

static const embi_sel_list *parse_list(sp *p, const embi_css_cv *v, uint32_t n, bool forgiving,
                                       bool relative)
{
    uint32_t segs = 1;
    for (uint32_t k = 0; k < n; k++) {
        if (v[k].tok.type == EMBI_CSS_COMMA) {
            segs++;
        }
    }
    embi_sel_complex *items = embi_arena_alloc(p->tmp, (size_t)segs * sizeof *items, 8);
    uint32_t m = 0;
    uint32_t start = 0;

    for (uint32_t k = 0; k <= n; k++) {
        if (k < n && v[k].tok.type != EMBI_CSS_COMMA) {
            continue;
        }
        uint32_t b = skip_ws(v, k, start);
        uint32_t e = k;
        while (e > b && v[e - 1].tok.type == EMBI_CSS_WS) {
            e--;
        }
        embi_sel_complex c;
        bool ok = e > b && parse_complex(p, v + b, e - b, relative, &c);
        if (ok) {
            items[m++] = c;
        } else if (!forgiving) {
            return nullptr;
        }
        start = k + 1;
    }

    embi_sel_list *l = embi_arena_alloc(p->out, sizeof *l, 8);
    l->n = m;
    l->items = embi_css_arena_copy(p->out, items, m, sizeof *items);
    l->relative = relative;
    return l;
}

/* Parses a nested argument list (a pseudo-class argument): pseudo-elements are invalid inside. */
static const embi_sel_list *parse_arg_list(sp *p, const embi_css_cv *v, uint32_t n, bool forgiving,
                                           bool relative)
{
    if (p->depth >= EMBI_SEL_MAX_DEPTH) {
        p->too_deep = true;
        return nullptr;
    }
    bool saved = p->in_args;
    p->in_args = true;
    p->depth++;
    const embi_sel_list *l = parse_list(p, v, n, forgiving, relative);
    p->depth--;
    p->in_args = saved;
    return l;
}

static bool parse_nth(sp *p, const embi_css_cv *kids, uint32_t nk, uint8_t flags, bool allow_of,
                      embi_sel_simple *s)
{
    uint32_t of_at = nk;
    for (uint32_t j = 0; j < nk; j++) {
        if (kids[j].tok.type == EMBI_CSS_IDENT && ieq_lit(kids[j].tok.value, "of")) {
            of_at = j;
            break;
        }
    }
    if (of_at != nk && !allow_of) {
        return false;
    }
    int64_t a = 0;
    int64_t b = 0;
    if (!embi_sel_parse_anb(embi_css_slice_make(kids, of_at), &a, &b)) {
        return false;
    }
    s->kind = EMBI_SS_NTH;
    s->a = a;
    s->b = b;
    s->flags = flags;
    if (of_at != nk) {
        const embi_sel_list *l = parse_arg_list(p, kids + of_at + 1, nk - of_at - 1, false, false);
        if (l == nullptr || l->n == 0) {
            return false;
        }
        s->list = l;
    }
    return true;
}

static bool parse_lang(sp *p, const embi_css_cv *v, uint32_t n, embi_sel_simple *s)
{
    embi_str *tmpv = embi_arena_alloc(p->tmp, (size_t)(n + 1) * sizeof *tmpv, 8);
    uint32_t m = 0;
    uint32_t start = 0;
    for (uint32_t k = 0; k <= n; k++) {
        if (k < n && v[k].tok.type != EMBI_CSS_COMMA) {
            continue;
        }
        uint32_t b = skip_ws(v, k, start);
        uint32_t e = k;
        while (e > b && v[e - 1].tok.type == EMBI_CSS_WS) {
            e--;
        }
        if (e != b + 1) {
            return false;
        }
        if (v[b].tok.type != EMBI_CSS_IDENT && v[b].tok.type != EMBI_CSS_STRING) {
            return false;
        }
        tmpv[m++] = v[b].tok.value;
        start = k + 1;
    }
    embi_str *dst = embi_css_arena_copy(p->out, tmpv, m, sizeof *tmpv);
    for (uint32_t k = 0; k < m; k++) {
        dst[k] = copy_str(p->out, dst[k]);
    }
    s->kind = EMBI_SS_LANG;
    s->strs = dst;
    s->nstrs = m;
    return true;
}

static bool parse_pseudo_class_fn(sp *p, const embi_css_cv *c, embi_sel_simple *s)
{
    embi_str name = c->tok.value;
    const embi_css_cv *kids = c->kids;
    uint32_t nk = c->nkids;

    if (ieq_lit(name, "nth-child")) {
        return parse_nth(p, kids, nk, 0, true, s);
    }
    if (ieq_lit(name, "nth-last-child")) {
        return parse_nth(p, kids, nk, EMBI_SNF_FROM_END, true, s);
    }
    if (ieq_lit(name, "nth-of-type")) {
        return parse_nth(p, kids, nk, EMBI_SNF_OF_TYPE, false, s);
    }
    if (ieq_lit(name, "nth-last-of-type")) {
        return parse_nth(p, kids, nk, EMBI_SNF_OF_TYPE | EMBI_SNF_FROM_END, false, s);
    }
    if (ieq_lit(name, "is") || ieq_lit(name, "where")) {
        const embi_sel_list *l = parse_arg_list(p, kids, nk, true, false);
        if (l == nullptr) {
            return false;
        }
        s->kind = ieq_lit(name, "is") ? EMBI_SS_IS : EMBI_SS_WHERE;
        s->list = l;
        return true;
    }
    if (ieq_lit(name, "not")) {
        const embi_sel_list *l = parse_arg_list(p, kids, nk, false, false);
        if (l == nullptr || l->n == 0) {
            return false;
        }
        s->kind = EMBI_SS_NOT;
        s->list = l;
        return true;
    }
    if (ieq_lit(name, "has")) {
        if (p->in_has) {
            return false;
        }
        bool saved = p->in_has;
        p->in_has = true;
        const embi_sel_list *l = parse_arg_list(p, kids, nk, false, true);
        p->in_has = saved;
        if (l == nullptr || l->n == 0) {
            return false;
        }
        s->kind = EMBI_SS_HAS;
        s->list = l;
        return true;
    }
    if (ieq_lit(name, "lang")) {
        return parse_lang(p, kids, nk, s);
    }
    if (ieq_lit(name, "dir")) {
        uint32_t i = skip_ws(kids, nk, 0);
        if (i >= nk || kids[i].tok.type != EMBI_CSS_IDENT) {
            return false;
        }
        uint8_t d = 0;
        if (ieq_lit(kids[i].tok.value, "ltr")) {
            d = EMBI_DIR_LTR;
        } else if (ieq_lit(kids[i].tok.value, "rtl")) {
            d = EMBI_DIR_RTL;
        } else {
            return false;
        }
        if (skip_ws(kids, nk, i + 1) != nk) {
            return false;
        }
        s->kind = EMBI_SS_DIR;
        s->sub = d;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ entry points */

static const embi_sel_list *parse_top(embi_arena *out, embi_css_slice in, const embi_sel_env *env,
                                      bool relative)
{
    embi_arena tmp;
    embi_sel_lower_cache_reset();
    embi_arena_init(&tmp, 4096);
    sp p = {out, &tmp, env, false, false, 0, false};
    const embi_sel_list *l = parse_list(&p, in.p, in.n, false, relative);
    embi_arena_free(&tmp);
    return p.too_deep ? nullptr : l;
}

const embi_sel_list *embi_sel_parse(embi_arena *a, embi_css_slice in, const embi_sel_env *env)
{
    return parse_top(a, in, env, false);
}

const embi_sel_list *embi_sel_parse_relative(embi_arena *a, embi_css_slice in,
                                             const embi_sel_env *env)
{
    return parse_top(a, in, env, true);
}

const embi_sel_list *embi_sel_parse_text(embi_arena *a, embi_str text, const embi_sel_env *env)
{
    embi_css_slice in = embi_css_parse_cvs(a, text);
    return parse_top(a, in, env, false);
}
