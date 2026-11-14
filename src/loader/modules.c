/*
 * The module host (W7d, modules.h): the engine's module loader, the import maps and import.meta over the loader's URL
 * parser and MIME rules. Every request of the engine is answered synchronously: the source comes from the document's
 * module cache (the bytes of every module fetched so far, kept until the document goes), a pending inline module, or a
 * load through the embedder (mod_host.load). A module whose media type is not JavaScript (or JSON for an import with
 * type "json") fails its request, as browsers do.
 *
 * Import maps follow the WHATWG import maps rules: one map per script element, merged into the document's maps (the
 * first entry of a key wins), scopes matched by the longest prefix of the referrer's URL, prefix keys ending in "/", and
 * null entries for values that are not URLs (a specifier that maps to one fails). A map that comes after the first
 * module load is ignored.
 */
#include "modules.h"

#include "loader/loader.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

/* The fragment that marks an inline module's key (see the header). */
#define EMBI_INLINE_TAG "#ember-inline-"
#define EMBI_INLINE_TAG_LEN ((uint32_t)(sizeof EMBI_INLINE_TAG - 1))

typedef struct imap_entry {
    embi_sb key;   /* normalized: an absolute URL for URL-like keys */
    embi_sb value; /* an absolute URL, or empty when null */
    bool null;     /* the entry maps to nothing: a specifier that matches it fails */
} imap_entry;
typedef EMBI_VEC(imap_entry) imap_entry_vec;

typedef struct imap_scope {
    embi_sb prefix; /* the absolute URL of the scope */
    imap_entry_vec imports;
} imap_scope;
typedef EMBI_VEC(imap_scope) imap_scope_vec;

typedef struct imap_data {
    imap_entry_vec top;
    imap_scope_vec scopes;
} imap_data;

/* A module's source by absolute URL: the document's module cache. */
typedef struct mcache {
    embi_sb key;
    bool json;
    uint8_t *data; /* SDL_malloc'd */
    size_t len;
} mcache;
typedef EMBI_VEC(mcache) mcache_vec;

/* An inline module's source, waiting for the engine's entry request for its key. */
typedef struct minline {
    embi_sb key;
    embi_sb src;
} minline;
typedef EMBI_VEC(minline) minline_vec;

/* An engine request whose module is still downloading: answered by embi_modules_retry once the host has it. */
typedef struct mpend {
    uint32_t request;
    embi_sb key; /* the absolute URL */
    bool json;
} mpend;
typedef EMBI_VEC(mpend) mpend_vec;

struct embi_modules;

/* The record of one import.meta: the key of its module, for import.meta.resolve. */
typedef struct meta_rec {
    struct embi_modules *m;
    embi_sb key;
} meta_rec;
typedef EMBI_VEC(meta_rec *) meta_vec;

struct embi_modules {
    embi_js_runtime *rt;
    embi_js_realm *realm;
    embi_mod_host host;
    embi_sb doc_url;
    embi_sb script_url;
    imap_data maps;
    bool frozen; /* a module was requested: import maps are closed */
    mcache_vec cache;
    minline_vec inl;
    meta_vec metas;
    mpend_vec pend;
    uint32_t inline_seq;
};

/* ------------------------------------------------------------------ small helpers */

static embi_str sb_str(const embi_sb *sb)
{
    return (embi_str){sb->data, sb->len};
}

static embi_str cstr_str(const char *s)
{
    return (embi_str){s, (uint32_t)strlen(s)};
}

static void set_err(embi_sb *err, const char *text)
{
    embi_sb_append(err, cstr_str(text));
}

/* The part of s before its fragment. */
static embi_str no_fragment(embi_str s)
{
    for (uint32_t i = 0; i < s.len; i++) {
        if (s.ptr[i] == '#') {
            return (embi_str){s.ptr, i};
        }
    }
    return s;
}

/* The document URL an inline module's key stands for (the key without its "#ember-inline-N" part). */
static embi_str inline_document_url(embi_str key)
{
    if (key.len < EMBI_INLINE_TAG_LEN) {
        return key;
    }
    for (uint32_t i = 0; i + EMBI_INLINE_TAG_LEN <= key.len; i++) {
        if (memcmp(key.ptr + i, EMBI_INLINE_TAG, EMBI_INLINE_TAG_LEN) == 0) {
            return (embi_str){key.ptr, i};
        }
    }
    return key;
}

/* A specifier that is relative to a base: starts with "/", "./" or "../". */
static bool url_like(embi_str s)
{
    if (s.len == 0) {
        return false;
    }
    if (s.ptr[0] == '/') {
        return true;
    }
    if (s.len >= 2 && s.ptr[0] == '.' && s.ptr[1] == '/') {
        return true;
    }
    return s.len >= 3 && s.ptr[0] == '.' && s.ptr[1] == '.' && s.ptr[2] == '/';
}

/* Parses s with base (nullptr: an absolute URL only). out is zeroed on failure. */
static bool parse_url(embi_str s, const embi_url *base, embi_url *out)
{
    *out = (embi_url){0};
    return embi_url_parse(s, base, out);
}

/* Parses base into b when it is a URL (the caller frees b). */
static bool parse_base(embi_str base, embi_url *b)
{
    return base.len > 0 && parse_url(base, nullptr, b);
}

/* Resolves s against base into out (an absolute URL). False when s is not a URL against base. */
static bool resolve_against(embi_str base, embi_str s, embi_sb *out)
{
    embi_url b = {0};
    bool hb = parse_base(base, &b);
    embi_url u = {0};
    bool ok = hb && parse_url(s, &b, &u);
    if (hb) {
        embi_url_free(&b);
    }
    if (ok) {
        embi_sb_append(out, (embi_str){u.href, u.href_len});
        embi_url_free(&u);
    }
    return ok;
}

/* An absolute URL or a relative one, resolved against base. */
static bool any_url(embi_str base, embi_str s, embi_sb *out)
{
    if (url_like(s)) {
        return resolve_against(base, s, out);
    }
    embi_url u = {0};
    if (parse_url(s, nullptr, &u)) {
        embi_sb_append(out, (embi_str){u.href, u.href_len});
        embi_url_free(&u);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ import maps: the data */

static void entry_free(imap_entry *e)
{
    embi_sb_free(&e->key);
    embi_sb_free(&e->value);
}

static void entries_free(imap_entry_vec *v)
{
    for (uint32_t i = 0; i < v->len; i++) {
        entry_free(&v->data[i]);
    }
    embi_vec_free(v);
}

static void maps_free(imap_data *d)
{
    entries_free(&d->top);
    for (uint32_t i = 0; i < d->scopes.len; i++) {
        embi_sb_free(&d->scopes.data[i].prefix);
        entries_free(&d->scopes.data[i].imports);
    }
    embi_vec_free(&d->scopes);
}

/* Appends an entry. A key the list already has is replaced when replace is true, else kept. */
static void entries_add(imap_entry_vec *v, embi_str key, embi_str value, bool null, bool replace)
{
    for (uint32_t i = 0; i < v->len; i++) {
        imap_entry *e = &v->data[i];
        if (embi_str_eq(sb_str(&e->key), key)) {
            if (replace) {
                embi_sb_clear(&e->value);
                embi_sb_append(&e->value, value);
                e->null = null;
            }
            return;
        }
    }
    imap_entry e = {{nullptr, 0, 0}, {nullptr, 0, 0}, null};
    embi_sb_append(&e.key, key);
    embi_sb_append(&e.value, value);
    embi_vec_push(v, e);
}

/* Normalizes an import map key: a URL-like key becomes its absolute URL, anything else stays as it is. */
static void key_normalize(embi_str base, embi_str key, embi_sb *out)
{
    if (!any_url(base, key, out)) {
        embi_sb_append(out, key);
    }
}

/* ------------------------------------------------------------------ JSON for import maps */

typedef struct jin {
    const unsigned char *p;
    const unsigned char *end;
} jin;

static void jws(jin *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static bool jchar(jin *j, unsigned char c)
{
    jws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return true;
    }
    return false;
}

static int hexdigit(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool jhex4(jin *j, uint32_t *v)
{
    if (j->end - j->p < 4) {
        return false;
    }
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        int d = hexdigit(j->p[i]);
        if (d < 0) {
            return false;
        }
        r = r * 16u + (uint32_t)d;
    }
    j->p += 4;
    *v = r;
    return true;
}

/* A JSON string (escapes decoded to UTF-8) appended to out. */
static bool jstring(jin *j, embi_sb *out)
{
    if (!jchar(j, '"')) {
        return false;
    }
    while (j->p < j->end) {
        unsigned char c = *j->p++;
        if (c == '"') {
            return true;
        }
        if (c < 0x20) {
            return false;
        }
        if (c != '\\') {
            embi_sb_append_char(out, (char)c);
            continue;
        }
        if (j->p >= j->end) {
            return false;
        }
        c = *j->p++;
        switch (c) {
        case '"':
        case '\\':
        case '/':
            embi_sb_append_char(out, (char)c);
            break;
        case 'b':
            embi_sb_append_char(out, '\b');
            break;
        case 'f':
            embi_sb_append_char(out, '\f');
            break;
        case 'n':
            embi_sb_append_char(out, '\n');
            break;
        case 'r':
            embi_sb_append_char(out, '\r');
            break;
        case 't':
            embi_sb_append_char(out, '\t');
            break;
        case 'u': {
            uint32_t u = 0;
            if (!jhex4(j, &u)) {
                return false;
            }
            if (u >= 0xD800 && u <= 0xDBFF) {
                const unsigned char *save = j->p;
                uint32_t lo = 0;
                if (j->end - j->p >= 2 && j->p[0] == '\\' && j->p[1] == 'u') {
                    j->p += 2;
                    if (jhex4(j, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
                    } else {
                        j->p = save;
                        u = 0xFFFD;
                    }
                } else {
                    u = 0xFFFD;
                }
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                u = 0xFFFD;
            }
            embi_sb_append_cp(out, u);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

/* Skips a JSON string without keeping it. */
static bool jskip_string(jin *j)
{
    embi_sb tmp = {nullptr, 0, 0};
    bool ok = jstring(j, &tmp);
    embi_sb_free(&tmp);
    return ok;
}

/* Skips any JSON value (depth limits the nesting). */
static bool jskip_value(jin *j, int depth)
{
    if (depth > 64) {
        return false;
    }
    jws(j);
    if (j->p >= j->end) {
        return false;
    }
    unsigned char c = *j->p;
    if (c == '"') {
        return jskip_string(j);
    }
    if (c == '{' || c == '[') {
        j->p++;
        unsigned char close = c == '{' ? '}' : ']';
        if (jchar(j, close)) {
            return true;
        }
        do {
            if (c == '{') {
                if (!jskip_string(j) || !jchar(j, ':')) {
                    return false;
                }
            }
            if (!jskip_value(j, depth + 1)) {
                return false;
            }
        } while (jchar(j, ','));
        return jchar(j, close);
    }
    const unsigned char *start = j->p;
    while (j->p < j->end) {
        unsigned char d = *j->p;
        bool word = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || (d >= '0' && d <= '9') || d == '-' ||
                    d == '+' || d == '.';
        if (!word) {
            break;
        }
        j->p++;
    }
    return j->p != start;
}

/* One imports object: its string values become entries of list (keys normalized against base). Other values are
 * skipped. False on a syntax error or when the value is not an object. */
static bool parse_imports(jin *j, embi_str base, imap_entry_vec *list, embi_sb *err)
{
    if (!jchar(j, '{')) {
        set_err(err, "an \"imports\" value is not an object");
        return false;
    }
    if (jchar(j, '}')) {
        return true;
    }
    do {
        embi_sb key = {nullptr, 0, 0};
        embi_sb value = {nullptr, 0, 0};
        bool ok = jstring(j, &key) && jchar(j, ':');
        jws(j);
        if (ok && j->p < j->end && *j->p == '"') {
            ok = jstring(j, &value);
            if (ok) {
                embi_sb nkey = {nullptr, 0, 0};
                embi_sb nval = {nullptr, 0, 0};
                key_normalize(base, sb_str(&key), &nkey);
                bool has_value = any_url(base, sb_str(&value), &nval);
                bool trailing = nkey.len > 0 && nkey.data[nkey.len - 1] == '/';
                bool null = !has_value || (trailing && !(nval.len > 0 && nval.data[nval.len - 1] == '/'));
                entries_add(list, sb_str(&nkey), sb_str(&nval), null, true);
                embi_sb_free(&nkey);
                embi_sb_free(&nval);
            }
        } else if (ok) {
            ok = jskip_value(j, 1);
        }
        embi_sb_free(&key);
        embi_sb_free(&value);
        if (!ok) {
            set_err(err, "the import map has a syntax error");
            return false;
        }
    } while (jchar(j, ','));
    if (!jchar(j, '}')) {
        set_err(err, "the import map has a syntax error");
        return false;
    }
    return true;
}

/* The scopes object: each key is a scope prefix (a URL, resolved against base) whose value is an imports object. A
 * prefix that is not a URL drops its scope. */
static bool parse_scopes(jin *j, embi_str base, imap_scope_vec *scopes, embi_sb *err)
{
    if (!jchar(j, '{')) {
        set_err(err, "a \"scopes\" value is not an object");
        return false;
    }
    if (jchar(j, '}')) {
        return true;
    }
    do {
        embi_sb key = {nullptr, 0, 0};
        if (!jstring(j, &key) || !jchar(j, ':')) {
            embi_sb_free(&key);
            set_err(err, "the import map has a syntax error");
            return false;
        }
        imap_scope scope = {{nullptr, 0, 0}, {nullptr, 0, 0}};
        bool url_ok = any_url(base, sb_str(&key), &scope.prefix);
        embi_sb_free(&key);
        bool ok = parse_imports(j, base, &scope.imports, err);
        if (!ok || !url_ok) {
            embi_sb_free(&scope.prefix);
            entries_free(&scope.imports);
            if (!ok) {
                return false;
            }
            continue;
        }
        embi_vec_push(scopes, scope);
    } while (jchar(j, ','));
    if (!jchar(j, '}')) {
        set_err(err, "the import map has a syntax error");
        return false;
    }
    return true;
}

/* Parses one import map text into out (zeroed by the caller). The text is a JSON object with "imports" and "scopes";
 * other members, such as "integrity", are skipped. */
static bool parse_map(embi_str text, embi_str base, imap_data *out, embi_sb *err)
{
    if (text.len == 0) {
        set_err(err, "the import map is empty");
        return false;
    }
    jin j = {(const unsigned char *)text.ptr, (const unsigned char *)text.ptr + text.len};
    if (!jchar(&j, '{')) {
        set_err(err, "the import map is not an object");
        return false;
    }
    if (!jchar(&j, '}')) {
        do {
            embi_sb key = {nullptr, 0, 0};
            if (!jstring(&j, &key) || !jchar(&j, ':')) {
                embi_sb_free(&key);
                set_err(err, "the import map has a syntax error");
                return false;
            }
            bool ok;
            if (embi_str_eq(sb_str(&key), EMBI_STR("imports"))) {
                ok = parse_imports(&j, base, &out->top, err);
            } else if (embi_str_eq(sb_str(&key), EMBI_STR("scopes"))) {
                ok = parse_scopes(&j, base, &out->scopes, err);
            } else {
                ok = jskip_value(&j, 1);
                if (!ok) {
                    set_err(err, "the import map has a syntax error");
                }
            }
            embi_sb_free(&key);
            if (!ok) {
                return false;
            }
        } while (jchar(&j, ','));
        if (!jchar(&j, '}')) {
            set_err(err, "the import map has a syntax error");
            return false;
        }
    }
    jws(&j);
    if (j.p != j.end) {
        set_err(err, "the import map has text after its object");
        return false;
    }
    return true;
}

/* Moves the entries of src that dst does not have yet into dst (the first map wins); src is emptied. */
static void merge_entries(imap_entry_vec *dst, imap_entry_vec *src)
{
    for (uint32_t i = 0; i < src->len; i++) {
        imap_entry *e = &src->data[i];
        bool present = false;
        for (uint32_t k = 0; k < dst->len && !present; k++) {
            present = embi_str_eq(sb_str(&dst->data[k].key), sb_str(&e->key));
        }
        if (present) {
            entry_free(e);
        } else {
            embi_vec_push(dst, *e);
        }
    }
    src->len = 0;
}

/* Merges src into dst: the top-level entries, and the scopes (a scope both maps have gets the merged imports). */
static void merge_maps(imap_data *dst, imap_data *src)
{
    merge_entries(&dst->top, &src->top);
    for (uint32_t i = 0; i < src->scopes.len; i++) {
        imap_scope *s = &src->scopes.data[i];
        imap_scope *target = nullptr;
        for (uint32_t k = 0; k < dst->scopes.len && target == nullptr; k++) {
            if (embi_str_eq(sb_str(&dst->scopes.data[k].prefix), sb_str(&s->prefix))) {
                target = &dst->scopes.data[k];
            }
        }
        if (target != nullptr) {
            merge_entries(&target->imports, &s->imports);
            embi_sb_free(&s->prefix);
            entries_free(&s->imports);
        } else {
            embi_vec_push(&dst->scopes, *s);
        }
    }
    src->scopes.len = 0;
}

/* ------------------------------------------------------------------ resolution */

/* Whether a scope applies to the base URL: the same URL, or a directory prefix of it. */
static bool scope_applies(embi_str prefix, embi_str base)
{
    if (prefix.len == 0) {
        return false;
    }
    if (embi_str_eq(prefix, base)) {
        return true;
    }
    return prefix.ptr[prefix.len - 1] == '/' && base.len >= prefix.len && memcmp(base.ptr, prefix.ptr, prefix.len) == 0;
}

/* One map against a normalized specifier. 1: found (out has the URL), 0: no entry applies, -1: a null entry or a value
 * that is not a URL (err has the message). */
static int match_entries(const imap_entry_vec *list, embi_str norm, embi_sb *out, embi_sb *err)
{
    for (uint32_t i = 0; i < list->len; i++) {
        const imap_entry *e = &list->data[i];
        if (embi_str_eq(sb_str(&e->key), norm)) {
            if (e->null) {
                set_err(err, "the import map maps this specifier to nothing");
                return -1;
            }
            embi_sb_append(out, sb_str(&e->value));
            return 1;
        }
    }
    const imap_entry *best = nullptr;
    for (uint32_t i = 0; i < list->len; i++) {
        const imap_entry *e = &list->data[i];
        embi_str key = sb_str(&e->key);
        if (key.len == 0 || key.len >= norm.len || key.ptr[key.len - 1] != '/') {
            continue;
        }
        if (memcmp(norm.ptr, key.ptr, key.len) != 0) {
            continue;
        }
        if (best == nullptr || key.len > sb_str(&best->key).len) {
            best = e;
        }
    }
    if (best == nullptr) {
        return 0;
    }
    if (best->null) {
        set_err(err, "the import map maps this specifier to nothing");
        return -1;
    }
    embi_sb joined = {nullptr, 0, 0};
    embi_sb_append(&joined, sb_str(&best->value));
    uint32_t klen = sb_str(&best->key).len;
    embi_str rest = {norm.ptr + klen, norm.len - klen};
    embi_sb_append(&joined, rest);
    int r = -1;
    if (any_url((embi_str){nullptr, 0}, sb_str(&joined), out)) {
        r = 1;
    } else {
        set_err(err, "the import map maps this specifier to an invalid URL");
    }
    embi_sb_free(&joined);
    return r;
}

/* The specifier resolved against base through the import maps (scopes first, then the top-level imports), or as the URL
 * it is. The result is appended to out; the message to err. */
static bool resolve_in(embi_js_runtime *rt, embi_modules *m, embi_str spec, embi_str base, embi_sb *out, embi_sb *err)
{
    (void)rt;
    embi_url bu = {0};
    bool have_base = parse_base(base, &bu);
    embi_url as = {0};
    bool have_as = url_like(spec) ? (have_base && parse_url(spec, &bu, &as)) : parse_url(spec, nullptr, &as);
    embi_str norm = have_as ? (embi_str){as.href, as.href_len} : spec;
    embi_str base_href = have_base ? (embi_str){bu.href, bu.href_len} : base;
    int r = 0;
    /* The applicable scopes are tried in order of decreasing prefix length; the first that yields a result (a match or a
     * null entry) decides. Prefixes are unique (merged), so a strict length bound visits each scope once. */
    uint32_t bound = UINT32_MAX;
    while (r == 0) {
        const imap_scope *scope = nullptr;
        for (uint32_t i = 0; i < m->maps.scopes.len; i++) {
            const imap_scope *s = &m->maps.scopes.data[i];
            if (s->prefix.len < bound && scope_applies(sb_str(&s->prefix), base_href) &&
                (scope == nullptr || s->prefix.len > scope->prefix.len)) {
                scope = s;
            }
        }
        if (scope == nullptr) {
            break;
        }
        bound = scope->prefix.len;
        r = match_entries(&scope->imports, norm, out, err);
    }
    if (r == 0) {
        r = match_entries(&m->maps.top, norm, out, err);
    }
    if (r == 0) {
        if (have_as) {
            embi_sb_append(out, norm);
            r = 1;
        } else {
            set_err(err, "Failed to resolve module specifier \"");
            embi_sb_append(err, spec);
            set_err(err, "\": a bare specifier needs an import map entry");
            r = -1;
        }
    }
    if (have_as) {
        embi_url_free(&as);
    }
    if (have_base) {
        embi_url_free(&bu);
    }
    return r == 1;
}

bool embi_modules_resolve(embi_modules *m, embi_str specifier, embi_str base, embi_sb *out, embi_sb *err)
{
    return resolve_in(m->rt, m, specifier, base, out, err);
}

/* ------------------------------------------------------------------ the module cache and the loads */

static mcache *cache_find(embi_modules *m, embi_str key, bool json)
{
    for (uint32_t i = 0; i < m->cache.len; i++) {
        mcache *c = &m->cache.data[i];
        if (c->json == json && embi_str_eq(sb_str(&c->key), key)) {
            return c;
        }
    }
    return nullptr;
}

/* Whether a media type is one a module (json: a JSON module) may have. With no media type, the URL's extension decides. */
static bool mime_ok(embi_str mime, embi_str url, bool json)
{
    embi_str src = mime.len > 0 ? mime : embi_mime_from_path(url);
    if (src.len == 0) {
        return false;
    }
    embi_mime mt = {0};
    bool ok = false;
    if (embi_mime_parse(src, &mt)) {
        embi_sb ess = {nullptr, 0, 0};
        embi_mime_essence(&mt, &ess);
        embi_str e = sb_str(&ess);
        ok = json ? embi_mime_essence_is_json(e) : embi_mime_essence_is_javascript(e);
        embi_sb_free(&ess);
    }
    embi_mime_free(&mt);
    return ok;
}

/* Hands the static imports of a module that has just arrived to the host's prefetch, so a tree of modules downloads in
 * parallel instead of one level after the other. A lexical guess: the specifier strings after "from" and after "import"
 * (at most 64); a wrong guess costs one download. */
static void prefetch_imports(embi_modules *m, embi_str base, const uint8_t *src, size_t len)
{
    if (m->host.prefetch == nullptr) {
        return;
    }
    uint32_t found = 0;
    for (size_t i = 0; i + 4 < len && found < 64; i++) {
        size_t k;
        /* FP2a: the first byte decides almost every position (this used to call strncmp twice per byte of the
         * source: about 0.7 ms for the 223 KB react-todomvc bundle). */
        const uint8_t c0 = src[i];
        if (c0 == 'f' && src[i + 1] == 'r' && src[i + 2] == 'o' && src[i + 3] == 'm') {
            k = i + 4;
        } else if (c0 == 'i' && i + 6 < len && src[i + 1] == 'm' && src[i + 2] == 'p' && src[i + 3] == 'o' &&
                   src[i + 4] == 'r' && src[i + 5] == 't') {
            k = i + 6;
        } else {
            continue;
        }
        while (k < len && (src[k] == ' ' || src[k] == 9 || src[k] == 10 || src[k] == 13)) {
            k++;
        }
        if (k >= len || (src[k] != 34 && src[k] != 39)) {
            continue;
        }
        uint8_t q = src[k++];
        size_t e = k;
        while (e < len && src[e] != q && src[e] != 10 && e - k < 512) {
            e++;
        }
        if (e >= len || src[e] != q || e == k) {
            continue;
        }
        embi_sb out = {nullptr, 0, 0};
        embi_sb err = {nullptr, 0, 0};
        if (resolve_in(m->rt, m, (embi_str){(const char *)src + k, (uint32_t)(e - k)}, base, &out, &err) && out.len > 0 &&
            cache_find(m, sb_str(&out), false) == nullptr) {
            m->host.prefetch(m->host.user, sb_str(&out));
            found++;
        }
        embi_sb_free(&out);
        embi_sb_free(&err);
        i = e;
    }
}

/* Loads url (absolute) through the host and keeps the source in the cache. A failure leaves the reason in msg. */
static bool fetch_module(embi_modules *m, embi_str url, bool json, embi_sb *msg)
{
    uint8_t *data = nullptr;
    size_t len = 0;
    embi_sb mime = {nullptr, 0, 0};
    if (m->host.load == nullptr || !m->host.load(m->host.user, url, &data, &len, &mime)) {
        set_err(msg, "Failed to fetch the module ");
        embi_sb_append(msg, url);
        SDL_free(data);
        embi_sb_free(&mime);
        return false;
    }
    if (!mime_ok(sb_str(&mime), url, json)) {
        set_err(msg, "Failed to load module script: ");
        embi_sb_append(msg, url);
        set_err(msg, " has a media type that is not a JavaScript module type");
        SDL_free(data);
        embi_sb_free(&mime);
        return false;
    }
    embi_sb_free(&mime);
    mcache entry = {{nullptr, 0, 0}, json, data, len};
    embi_sb_append(&entry.key, url);
    embi_vec_push(&m->cache, entry);
    if (!json) {
        prefetch_imports(m, url, data, len);
    }
    return true;
}

bool embi_modules_preload(embi_modules *m, embi_str url)
{
    if (cache_find(m, url, false) != nullptr) {
        return true;
    }
    embi_sb msg = {nullptr, 0, 0};
    bool ok = fetch_module(m, url, false, &msg);
    embi_sb_free(&msg);
    return ok;
}

/* Completes an engine request with a source (the engine copies what it keeps). */
static void complete(embi_modules *m, uint32_t request, embi_str key, const uint8_t *src, size_t len)
{
    embi_js_module_loaded(m->rt, request, key.ptr, key.len, src != nullptr ? (const char *)src : "", len);
}

static void fail_request(embi_modules *m, uint32_t request, embi_sb *msg)
{
    embi_js_module_load_failed(m->rt, request, embi_sb_cstr(msg));
}

/* Answers an engine request for the module at key (absolute): from the cache, by loading it, or, when the host says the load is
 * still going on, by keeping the request until embi_modules_retry (the engine accepts the answer later). */
static void answer_request(embi_modules *m, uint32_t request, embi_str key, bool json)
{
    embi_sb msg = {nullptr, 0, 0};
    bool have = cache_find(m, key, json) != nullptr || fetch_module(m, key, json, &msg);
    if (!have && m->host.is_pending != nullptr) {
        /* The load and the pending question are two reads of a record the network thread completes at any moment: a module that
         * arrived in between failed the first read and passes the second. Pending means wait; not pending means the record is
         * final, so the load is tried once more and its answer stands. */
        if (m->host.is_pending(m->host.user, key)) {
            mpend p = {request, {nullptr, 0, 0}, json};
            embi_sb_append(&p.key, key);
            embi_vec_push(&m->pend, p);
            embi_sb_free(&msg);
            return;
        }
        embi_sb_clear(&msg);
        have = fetch_module(m, key, json, &msg);
    }
    if (have) {
        mcache *c = cache_find(m, key, json);
        complete(m, request, key, c->data, c->len);
    } else {
        fail_request(m, request, &msg);
    }
    embi_sb_free(&msg);
}

/* The engine asks for a module: an inline module's source (an entry request whose key is one of ours), or the module at
 * the resolved URL. Each request is answered synchronously, so the engine never waits on the host. */
static void load_request(embi_js_runtime *rt, void *user, uint32_t request, const char *referrer, size_t rlen,
                         const char *specifier, size_t slen, const char *type)
{
    (void)rt;
    embi_modules *m = user;
    embi_str spec = {specifier, (uint32_t)slen};
    bool json = type != nullptr;
    embi_sb msg = {nullptr, 0, 0};
    if (json && !embi_str_eq(cstr_str(type), EMBI_STR("json"))) {
        set_err(&msg, "unsupported module type");
        fail_request(m, request, &msg);
        embi_sb_free(&msg);
        return;
    }
    if (referrer == nullptr) {
        for (uint32_t i = 0; i < m->inl.len; i++) {
            if (!embi_str_eq(sb_str(&m->inl.data[i].key), spec)) {
                continue;
            }
            /* Taken out of the list before the completion, which may run more requests. */
            minline taken = m->inl.data[i];
            m->inl.data[i] = m->inl.data[--m->inl.len];
            complete(m, request, spec, (const uint8_t *)taken.src.data, taken.src.len);
            embi_sb_free(&taken.key);
            embi_sb_free(&taken.src);
            return;
        }
    }
    m->frozen = true;
    embi_sb url = {nullptr, 0, 0};
    embi_str base = referrer != nullptr        ? (embi_str){referrer, (uint32_t)rlen}
                    : m->script_url.len > 0 ? sb_str(&m->script_url)
                                            : sb_str(&m->doc_url);
    if (!resolve_in(rt, m, spec, base, &url, &msg)) {
        fail_request(m, request, &msg);
        embi_sb_free(&url);
        embi_sb_free(&msg);
        return;
    }
    answer_request(m, request, sb_str(&url), json);
    embi_sb_free(&url);
    embi_sb_free(&msg);
}

bool embi_modules_pending(const embi_modules *m)
{
    return m != nullptr && m->pend.len > 0;
}

void embi_modules_retry(embi_modules *m)
{
    if (m == nullptr || m->pend.len == 0) {
        return;
    }
    /* The requests are taken out first: answering one can run the engine, which asks for more modules. */
    mpend_vec todo = m->pend;
    memset(&m->pend, 0, sizeof m->pend);
    for (uint32_t i = 0; i < todo.len; i++) {
        answer_request(m, todo.data[i].request, sb_str(&todo.data[i].key), todo.data[i].json);
        embi_sb_free(&todo.data[i].key);
    }
    embi_vec_free(&todo);
}

/* ------------------------------------------------------------------ import.meta */

static embi_js_value meta_resolve(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv,
                                  int magic)
{
    (void)this_v;
    (void)magic;
    meta_rec *rec = embi_js_native_data_current(r);
    embi_js_runtime *rt = r->rt;
    embi_js_string *s = embi_js_to_string(rt, argc > 0 ? argv[0] : EMBI_JS_UNDEFINED);
    if (s == nullptr) {
        return EMBI_JS_EXCEPTION;
    }
    embi_sb spec = {nullptr, 0, 0};
    embi_js_str_append_utf8(s, &spec);
    embi_sb out = {nullptr, 0, 0};
    embi_sb err = {nullptr, 0, 0};
    embi_js_value result = EMBI_JS_UNDEFINED;
    if (resolve_in(rt, rec->m, sb_str(&spec), sb_str(&rec->key), &out, &err)) {
        embi_js_string *o = embi_js_str_new_utf8(rt, out.data != nullptr ? out.data : "", out.len);
        if (o != nullptr) {
            result = embi_js_from_string(o);
        }
    } else {
        result = embi_js_throw_type_error(rt, "%s", embi_sb_cstr(&err));
    }
    embi_sb_free(&spec);
    embi_sb_free(&out);
    embi_sb_free(&err);
    return result;
}

static void meta_hook(embi_js_runtime *rt, embi_js_value meta, const char *key, size_t klen, void *user)
{
    embi_modules *m = user;
    embi_str k = {key, (uint32_t)klen};
    if (!embi_js_is_object(meta)) {
        return;
    }
    embi_js_object *o = embi_js_as_object(meta);
    embi_str url = inline_document_url(k);
    embi_js_string *s = embi_js_str_new_utf8(rt, url.ptr != nullptr ? url.ptr : "", url.len);
    if (s != nullptr) {
        (void)embi_js_put(rt, o, embi_js_key_c(rt, "url"), embi_js_from_string(s), false);
    }
    meta_rec *rec = embi_alloc(sizeof *rec);
    rec->m = m;
    rec->key = (embi_sb){nullptr, 0, 0};
    embi_sb_append(&rec->key, k);
    embi_vec_push(&m->metas, rec);
    embi_js_object *fn = embi_js_native_function_new_data(m->realm, "resolve", 1, meta_resolve, 0, rec);
    if (fn != nullptr) {
        (void)embi_js_put(rt, o, embi_js_key_c(rt, "resolve"), embi_js_from_object(fn), false);
    }
}

/* ------------------------------------------------------------------ the public API */

embi_modules *embi_modules_new(embi_js_runtime *rt, embi_js_realm *realm, const embi_mod_host *host)
{
    embi_modules *m = embi_alloc(sizeof *m);
    memset(m, 0, sizeof *m);
    m->rt = rt;
    m->realm = realm;
    m->host = *host;
    return m;
}

void embi_modules_free(embi_modules *m)
{
    if (m == nullptr) {
        return;
    }
    embi_sb_free(&m->doc_url);
    embi_sb_free(&m->script_url);
    maps_free(&m->maps);
    for (uint32_t i = 0; i < m->cache.len; i++) {
        embi_sb_free(&m->cache.data[i].key);
        SDL_free(m->cache.data[i].data);
    }
    embi_vec_free(&m->cache);
    for (uint32_t i = 0; i < m->inl.len; i++) {
        embi_sb_free(&m->inl.data[i].key);
        embi_sb_free(&m->inl.data[i].src);
    }
    embi_vec_free(&m->inl);
    for (uint32_t i = 0; i < m->metas.len; i++) {
        embi_sb_free(&m->metas.data[i]->key);
        embi_free(m->metas.data[i], sizeof(meta_rec));
    }
    embi_vec_free(&m->metas);
    for (uint32_t i = 0; i < m->pend.len; i++) {
        embi_sb_free(&m->pend.data[i].key);
    }
    embi_vec_free(&m->pend);
    embi_free(m, sizeof *m);
}

void embi_modules_attach(embi_modules *m)
{
    embi_js_set_module_loader(m->rt, load_request, m);
    embi_js_set_import_meta_hook(m->rt, meta_hook, m);
}

void embi_modules_set_doc_url(embi_modules *m, embi_str url)
{
    embi_sb_clear(&m->doc_url);
    embi_sb_append(&m->doc_url, no_fragment(url));
}

void embi_modules_set_script_url(embi_modules *m, embi_str url)
{
    embi_sb_clear(&m->script_url);
    embi_sb_append(&m->script_url, url);
}

bool embi_modules_add_importmap(embi_modules *m, embi_str text, embi_sb *err)
{
    if (m->frozen) {
        set_err(err, "an import map after a module was loaded is ignored");
        return false;
    }
    imap_data tmp = {{nullptr, 0, 0}, {nullptr, 0, 0}};
    if (!parse_map(text, sb_str(&m->doc_url), &tmp, err)) {
        maps_free(&tmp);
        return false;
    }
    merge_maps(&m->maps, &tmp);
    maps_free(&tmp);
    return true;
}

embi_js_value embi_modules_eval_url(embi_modules *m, embi_str url)
{
    m->frozen = true;
    return embi_js_module_eval(m->realm, url.ptr, url.len);
}

embi_js_value embi_modules_eval_source(embi_modules *m, embi_str src)
{
    embi_str base = sb_str(&m->doc_url);
    if (base.len == 0) {
        base = EMBI_STR("about:blank");
    }
    m->inline_seq++;
    minline e = {{nullptr, 0, 0}, {nullptr, 0, 0}};
    embi_sb_append(&e.key, base);
    embi_sb_append(&e.key, EMBI_STR(EMBI_INLINE_TAG));
    embi_sb_append_u64(&e.key, m->inline_seq);
    embi_sb_append(&e.src, src);
    embi_vec_push(&m->inl, e);
    /* The entry request carries the key as its specifier; the request takes the copy, the entry keeps its own. */
    embi_sb key = {nullptr, 0, 0};
    embi_sb_append(&key, sb_str(&e.key));
    embi_js_value p = embi_modules_eval_url(m, sb_str(&key));
    embi_sb_free(&key);
    return p;
}
