/*
 * Frontmatter of a test262 file. The runner reads includes, flags, features and negative from the
 * YAML subset that test262 uses: top-level keys at column 0, with a value that is either inline
 * (`[a, b]`, possibly wrapped over several lines) or a block of indented lines (`- item` lists and
 * `key: value` pairs).
 */
#include "test262.h"

#include <SDL3/SDL_stdinc.h>

#define FM_MAX_LINES 256
#define FM_MAX_ITEMS 64
#define FM_JOIN_MAX 2048

typedef struct fm_lines {
    const char *text[FM_MAX_LINES];
    size_t len[FM_MAX_LINES];
    int count;
} fm_lines;

typedef struct fm_items {
    char item[FM_MAX_ITEMS][T262_NAME_MAX];
    int count;
    bool overflow;
} fm_items;

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static bool is_indented(const char *s, size_t n)
{
    return n > 0 && (s[0] == ' ' || s[0] == '\t');
}

static bool is_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!is_space(s[i])) {
            return false;
        }
    }
    return true;
}

/* A top-level key line: starts at column 0, is not a comment or list item, and holds a colon. */
static bool is_top_key(const char *s, size_t n)
{
    if (n == 0 || s[0] == ' ' || s[0] == '\t' || s[0] == '-' || s[0] == '#') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] == ':') {
            return true;
        }
    }
    return false;
}

static void trim(const char **s, size_t *n)
{
    while (*n > 0 && is_space((*s)[0])) {
        (*s)++;
        (*n)--;
    }
    while (*n > 0 && is_space((*s)[*n - 1])) {
        (*n)--;
    }
}

static bool key_is(const char *k, size_t n, const char *name)
{
    return n == SDL_strlen(name) && SDL_memcmp(k, name, n) == 0;
}

static void copy_str(char *dst, size_t cap, const char *s, size_t n)
{
    if (cap == 0) {
        return;
    }
    size_t k = n < cap - 1 ? n : cap - 1;
    SDL_memcpy(dst, s, k);
    dst[k] = '\0';
}

static const char *find_bytes(const char *s, size_t len, const char *needle)
{
    size_t n = SDL_strlen(needle);
    if (n > len) {
        return nullptr;
    }
    for (size_t i = 0; i + n <= len; i++) {
        if (SDL_memcmp(s + i, needle, n) == 0) {
            return s + i;
        }
    }
    return nullptr;
}

static void split_lines(const char *s, size_t len, fm_lines *out)
{
    out->count = 0;
    size_t i = 0;
    while (i < len && out->count < FM_MAX_LINES) {
        size_t start = i;
        while (i < len && s[i] != '\n' && s[i] != '\r') {
            i++;
        }
        size_t end = i;
        out->text[out->count] = s + start;
        out->len[out->count] = end - start;
        out->count++;
        if (i < len) {
            /* LF, CR or CRLF ends a line (a few test files use bare CR). */
            if (s[i] == '\r' && i + 1 < len && s[i + 1] == '\n') {
                i++;
            }
            i++;
        }
    }
}

/* Adds one list item, trimmed and without surrounding quotes. */
static void add_item(fm_items *items, const char *s, size_t n)
{
    trim(&s, &n);
    if (n >= 2 && (s[0] == '\'' || s[0] == '"') && s[n - 1] == s[0]) {
        s++;
        n -= 2;
    }
    if (n == 0) {
        return;
    }
    if (items->count >= FM_MAX_ITEMS || n >= T262_NAME_MAX) {
        items->overflow = true;
        return;
    }
    copy_str(items->item[items->count], T262_NAME_MAX, s, n);
    items->count++;
}

/* Reads the items of a list-valued key: an inline flow list (`[a, b]`, joined over the block
 * lines when it wraps) or a block of `- item` lines. */
static void read_items(const char *val, size_t vlen, const fm_lines *L, int from, int to,
                       fm_items *items)
{
    items->count = 0;
    items->overflow = false;
    if (vlen > 0 && val[0] == '[') {
        char buf[FM_JOIN_MAX];
        size_t k = 0;
        for (size_t i = 0; i < vlen && k + 1 < sizeof buf; i++) {
            buf[k++] = val[i];
        }
        for (int j = from; j < to && k + 2 < sizeof buf; j++) {
            buf[k++] = ' ';
            for (size_t i = 0; i < L->len[j] && k + 2 < sizeof buf; i++) {
                buf[k++] = L->text[j][i];
            }
        }
        buf[k] = '\0';
        const char *p = buf + 1;
        const char *end = SDL_strchr(buf, ']');
        if (end == nullptr) {
            end = buf + k;
        }
        while (p < end) {
            const char *comma = p;
            while (comma < end && *comma != ',') {
                comma++;
            }
            add_item(items, p, (size_t)(comma - p));
            p = comma < end ? comma + 1 : end;
        }
        return;
    }
    for (int j = from; j < to; j++) {
        const char *s = L->text[j];
        size_t n = L->len[j];
        trim(&s, &n);
        if (n > 0 && s[0] == '-') {
            add_item(items, s + 1, n - 1);
        }
    }
}

static void apply_flag(t262_meta *m, const char *s, size_t n)
{
    trim(&s, &n);
    if (key_is(s, n, "onlyStrict")) {
        m->flags |= T262_FLAG_ONLY_STRICT;
    } else if (key_is(s, n, "noStrict")) {
        m->flags |= T262_FLAG_NO_STRICT;
    } else if (key_is(s, n, "raw")) {
        m->flags |= T262_FLAG_RAW;
    } else if (key_is(s, n, "async")) {
        m->flags |= T262_FLAG_ASYNC;
    } else if (key_is(s, n, "module")) {
        m->flags |= T262_FLAG_MODULE;
    } else if (key_is(s, n, "CanBlockIsTrue")) {
        m->flags |= T262_FLAG_CAN_BLOCK;
    }
}

static void read_negative(t262_meta *m, const fm_lines *L, int from, int to)
{
    m->has_negative = true;
    for (int j = from; j < to; j++) {
        const char *s = L->text[j];
        size_t n = L->len[j];
        trim(&s, &n);
        const char *colon = SDL_strchr(s, ':');
        if (colon == nullptr || colon >= s + n) {
            continue;
        }
        size_t kn = (size_t)(colon - s);
        const char *v = colon + 1;
        size_t vn = n - kn - 1;
        trim(&v, &vn);
        if (key_is(s, kn, "phase")) {
            copy_str(m->negative_phase, sizeof m->negative_phase, v, vn);
        } else if (key_is(s, kn, "type")) {
            copy_str(m->negative_type, sizeof m->negative_type, v, vn);
        }
    }
}

void t262_parse_meta(const char *src, size_t len, t262_meta *m)
{
    SDL_memset(m, 0, sizeof *m);
    const char *open = find_bytes(src, len, "/*---");
    if (open == nullptr) {
        return;
    }
    const char *body = open + 5;
    size_t body_len = (size_t)((src + len) - body);
    const char *close = find_bytes(body, body_len, "---*/");
    if (close == nullptr) {
        return;
    }
    fm_lines L;
    split_lines(body, (size_t)(close - body), &L);

    fm_items items;
    for (int i = 0; i < L.count; i++) {
        const char *s = L.text[i];
        size_t n = L.len[i];
        if (!is_top_key(s, n)) {
            continue;
        }
        size_t kn = 0;
        while (kn < n && s[kn] != ':') {
            kn++;
        }
        const char *val = s + kn + 1;
        size_t vn = n - kn - 1;
        trim(&val, &vn);

        int to = i + 1;
        while (to < L.count && (is_blank(L.text[to], L.len[to]) ||
                                is_indented(L.text[to], L.len[to]) ||
                                (L.len[to] > 0 && L.text[to][0] == '-'))) {
            to++;
        }
        int from = i + 1;

        if (key_is(s, kn, "includes")) {
            read_items(val, vn, &L, from, to, &items);
            for (int k = 0; k < items.count; k++) {
                if (m->n_includes < T262_MAX_INCLUDES) {
                    SDL_strlcpy(m->includes[m->n_includes++], items.item[k], T262_NAME_MAX);
                } else {
                    m->overflow = true;
                }
            }
            m->overflow = m->overflow || items.overflow;
        } else if (key_is(s, kn, "features")) {
            read_items(val, vn, &L, from, to, &items);
            for (int k = 0; k < items.count; k++) {
                if (m->n_features < T262_MAX_FEATURES) {
                    SDL_strlcpy(m->features[m->n_features++], items.item[k], T262_NAME_MAX);
                } else {
                    m->overflow = true;
                }
            }
            m->overflow = m->overflow || items.overflow;
        } else if (key_is(s, kn, "flags")) {
            read_items(val, vn, &L, from, to, &items);
            for (int k = 0; k < items.count; k++) {
                apply_flag(m, items.item[k], SDL_strlen(items.item[k]));
            }
        } else if (key_is(s, kn, "negative")) {
            read_negative(m, &L, from, to);
        }
        i = to - 1;
    }
}
