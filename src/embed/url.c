/*
 * URL helpers (W7d, see url.h). Resolution and the data: URLs are the loader's (src/loader/url.c, data.c: the WHATWG URL
 * parser, which replaced the RFC 3986 resolver of W4e); the file: paths and the bare paths stay here.
 */
#include "embed/url.h"

#include "loader/loader.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <string.h>

/* A growing NUL-terminated byte buffer (SDL_malloc'd). */
typedef struct ubuf {
    char *p;
    size_t len, cap;
} ubuf;

static void ub_init(ubuf *b)
{
    b->p = nullptr;
    b->len = 0;
    b->cap = 0;
}

static void ub_put(ubuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *p = SDL_realloc(b->p, cap);
        EMBI_ASSERT(p != nullptr);
        b->p = p;
        b->cap = cap;
    }
    if (n > 0) {
        SDL_memcpy(b->p + b->len, s, n);
    }
    b->len += n;
    b->p[b->len] = '\0';
}

static void ub_putc(ubuf *b, char c)
{
    ub_put(b, &c, 1);
}

static void ub_free(ubuf *b)
{
    SDL_free(b->p);
    b->p = nullptr;
    b->len = b->cap = 0;
}

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int hexval(char c)
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

bool embi_url_scheme(embi_str url, embi_str *scheme)
{
    if (url.len < 3 || !is_alpha(url.ptr[0])) {
        /* At least "s:x": a bare "C:" is never a scheme. */
        return false;
    }
    for (uint32_t i = 1; i < url.len; i++) {
        char c = url.ptr[i];
        if (c == ':') {
            /* A one-letter scheme is a drive letter when a slash follows ("C:/x", "C:\x"); otherwise it is a
             * scheme, as RFC 3986 has it ("g:h"). */
            if (i < 2 && (i + 1 >= url.len || url.ptr[i + 1] == '/' || url.ptr[i + 1] == '\\')) {
                return false;
            }
            if (scheme != nullptr) {
                *scheme = (embi_str){url.ptr, i};
            }
            return true;
        }
        if (!is_alpha(c) && !is_digit(c) && c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return false;
}

bool embi_url_scheme_is(embi_str scheme, const char *name)
{
    size_t n = strlen(name);
    if (scheme.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = scheme.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

/* The parts of a reference: scheme, authority (without "//"), path, query (without '?') and fragment (without '#').
 * Each is a view into the input. */
typedef struct uparts {
    embi_str scheme, auth, path, query, frag;
    bool has_scheme, has_auth, has_query, has_frag;
} uparts;

static uparts split(embi_str u)
{
    uparts p;
    memset(&p, 0, sizeof p);
    const char *s = u.ptr;
    uint32_t n = u.len, i = 0;
    if (embi_url_scheme(u, &p.scheme)) {
        p.has_scheme = true;
        i = p.scheme.len + 1;
    }
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
        uint32_t a = i + 2, e = a;
        while (e < n && s[e] != '/' && s[e] != '?' && s[e] != '#') {
            e++;
        }
        p.auth = (embi_str){s + a, e - a};
        p.has_auth = true;
        i = e;
    }
    uint32_t pe = i;
    while (pe < n && s[pe] != '?' && s[pe] != '#') {
        pe++;
    }
    p.path = (embi_str){s + i, pe - i};
    i = pe;
    if (i < n && s[i] == '?') {
        uint32_t qe = i + 1;
        while (qe < n && s[qe] != '#') {
            qe++;
        }
        p.query = (embi_str){s + i + 1, qe - i - 1};
        p.has_query = true;
        i = qe;
    }
    if (i < n && s[i] == '#') {
        p.frag = (embi_str){s + i + 1, n - i - 1};
        p.has_frag = true;
    }
    return p;
}

/* The reference rel resolved against base with the WHATWG URL parser. With no base, rel alone is parsed (an absolute URL
 * is normalized); a reference that does not parse is returned as it is. */
char *embi_url_resolve(embi_str base, embi_str rel, size_t *len)
{
    embi_url b = {0};
    embi_url r = {0};
    bool hb = base.len > 0 && embi_url_parse(base, nullptr, &b);
    bool ok = hb ? embi_url_parse(rel, &b, &r) : embi_url_parse(rel, nullptr, &r);
    embi_str out = ok ? (embi_str){r.href, r.href_len} : rel;
    char *p = SDL_malloc(out.len + 1);
    if (out.len > 0) {
        SDL_memcpy(p, out.ptr, out.len);
    }
    p[out.len] = '\0';
    *len = out.len;
    embi_url_free(&b);
    embi_url_free(&r);
    return p;
}

static bool is_unreserved(char c)
{
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':';
}

/* Percent-decodes n bytes of in into out. False for a malformed escape or an encoded NUL. */
static bool pct_decode(const char *in, size_t n, ubuf *out)
{
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c != '%') {
            ub_putc(out, c);
            continue;
        }
        if (i + 2 >= n) {
            return false;
        }
        int hi = hexval(in[i + 1]), lo = hexval(in[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        char d = (char)(hi * 16 + lo);
        if (d == '\0') {
            return false;
        }
        ub_putc(out, d);
        i += 2;
    }
    return true;
}

bool embi_url_to_path(embi_str url, char *out, size_t cap, size_t *len)
{
    embi_str scheme;
    bool has_scheme = embi_url_scheme(url, &scheme);
    if (has_scheme && !embi_url_scheme_is(scheme, "file")) {
        return false;
    }
    uparts p = split(url);
    if (has_scheme && p.has_auth && p.auth.len > 0 &&
        !(p.auth.len == 9 && SDL_strncasecmp(p.auth.ptr, "localhost", 9) == 0)) {
        return false;
    }
    ubuf raw;
    ub_init(&raw);
    if (!pct_decode(p.path.ptr, p.path.len, &raw)) {
        ub_free(&raw);
        return false;
    }
    for (size_t i = 0; i < raw.len; i++) {
        if (raw.p[i] == '\\') {
            raw.p[i] = '/';
        }
    }
    const char *s = raw.p;
    size_t n = raw.len;
    /* "/C:/x" is the drive path "C:/x". */
    if (n >= 3 && s[0] == '/' && is_alpha(s[1]) && s[2] == ':') {
        s++;
        n--;
    }
    /* A ".." segment is refused: the file loader never walks up from an allowed directory. */
    size_t seg = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            if (i - seg == 2 && s[seg] == '.' && s[seg + 1] == '.') {
                ub_free(&raw);
                return false;
            }
            seg = i + 1;
        }
    }
    bool ok = n > 0 && n + 1 <= cap;
    if (ok) {
        SDL_memcpy(out, s, n);
        out[n] = '\0';
        *len = n;
    }
    ub_free(&raw);
    return ok;
}

size_t embi_url_from_path(embi_str path, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    ubuf b;
    ub_init(&b);
    ub_put(&b, "file://", 7);
    bool drive = path.len >= 2 && is_alpha(path.ptr[0]) && path.ptr[1] == ':';
    if (drive || path.len == 0 || path.ptr[0] != '/' || path.ptr[0] == '\\') {
        ub_putc(&b, '/');
    }
    for (uint32_t i = 0; i < path.len; i++) {
        char c = path.ptr[i];
        if (c == '\\') {
            c = '/';
        }
        if (is_unreserved(c)) {
            ub_putc(&b, c);
        } else {
            char e[3] = {'%', hex[(uint8_t)c >> 4], hex[(uint8_t)c & 15]};
            ub_put(&b, e, 3);
        }
    }
    size_t need = b.len;
    if (cap > 0) {
        size_t k = need < cap - 1 ? need : cap - 1;
        SDL_memcpy(out, b.p, k);
        out[k] = '\0';
    }
    ub_free(&b);
    return need;
}

size_t embi_url_from_bare_path(embi_str path, char *out, size_t cap)
{
    bool drive = path.len >= 2 && is_alpha(path.ptr[0]) && path.ptr[1] == ':';
    bool absolute = drive || (path.len > 0 && (path.ptr[0] == '/' || path.ptr[0] == '\\'));
    char *cwd = absolute ? nullptr : SDL_GetCurrentDirectory();
    if (cwd == nullptr) {
        return embi_url_from_path(path, out, cap);
    }
    size_t cl = SDL_strlen(cwd);
    while (cl > 0 && (cwd[cl - 1] == '/' || cwd[cl - 1] == '\\')) {
        cl--;
    }
    size_t total = cl + 1 + path.len;
    char *joined = SDL_malloc(total + 1);
    if (joined == nullptr) {
        SDL_free(cwd);
        return embi_url_from_path(path, out, cap);
    }
    SDL_memcpy(joined, cwd, cl);
    joined[cl] = '/';
    SDL_memcpy(joined + cl + 1, path.ptr, path.len);
    joined[total] = '\0';
    size_t n = embi_url_from_path((embi_str){joined, (uint32_t)total}, out, cap);
    SDL_free(joined);
    SDL_free(cwd);
    return n;
}

bool embi_url_data(embi_str url, uint8_t **data, size_t *len)
{
    embi_str scheme;
    if (!embi_url_scheme(url, &scheme) || !embi_url_scheme_is(scheme, "data")) {
        return false;
    }
    embi_sb mime = {nullptr, 0, 0};
    embi_sb body = {nullptr, 0, 0};
    bool ok = embi_data_url(url, &mime, &body);
    embi_sb_free(&mime);
    if (ok) {
        uint8_t *buf = SDL_malloc(body.len > 0 ? body.len : 1);
        if (buf == nullptr) {
            ok = false;
        } else {
            if (body.len > 0) {
                SDL_memcpy(buf, body.data, body.len);
            }
            *data = buf;
            *len = body.len;
        }
    }
    embi_sb_free(&body);
    return ok;
}
