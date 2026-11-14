#pragma once
/*
 * Tiny JSON reader for test runners and generator tools. Header-only, C23, depends on libc only.
 *
 * Strings are UTF-8 bytes. \uXXXX escapes become UTF-8; a surrogate pair becomes one code point,
 * and a lone surrogate becomes its 3-byte (WTF-8) form. Objects keep their members in source
 * order and may contain duplicate keys; jm_get returns the first match.
 *
 * Usage:
 *     jm_val root;
 *     if (jm_parse(text, len, &root)) { ... jm_get(&root, "tests") ... }
 *     jm_free(&root);
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum jm_type { JM_NULL, JM_BOOL, JM_NUMBER, JM_STRING, JM_ARRAY, JM_OBJECT } jm_type;

typedef struct jm_val {
    jm_type type;
    bool b;
    double num;
    char *str;            /* JM_STRING: bytes, NUL-terminated */
    size_t len;           /* JM_STRING: byte length, without the terminator */
    struct jm_val *items; /* JM_ARRAY and JM_OBJECT: element or member values */
    char **keys;          /* JM_OBJECT: member keys, parallel to items (NULL entries in arrays) */
    size_t count;
    size_t cap;
} jm_val;

typedef struct jm_buf {
    char *p;
    size_t n;
    size_t cap;
} jm_buf;

typedef struct jm_parser {
    const char *p;
    const char *end;
} jm_parser;

static inline void jm_free(jm_val *v)
{
    if (v == nullptr) {
        return;
    }
    if (v->type == JM_STRING) {
        free(v->str);
    } else if (v->type == JM_ARRAY || v->type == JM_OBJECT) {
        for (size_t i = 0; i < v->count; i++) {
            jm_free(&v->items[i]);
            free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
    }
    memset(v, 0, sizeof *v);
}

static inline const jm_val *jm_get(const jm_val *obj, const char *key)
{
    if (obj == nullptr || obj->type != JM_OBJECT) {
        return nullptr;
    }
    for (size_t i = 0; i < obj->count; i++) {
        if (strcmp(obj->keys[i], key) == 0) {
            return &obj->items[i];
        }
    }
    return nullptr;
}

static inline bool jm_buf_put(jm_buf *b, unsigned char c)
{
    if (b->n + 1 >= b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        char *p = realloc(b->p, cap);
        if (p == nullptr) {
            return false;
        }
        b->p = p;
        b->cap = cap;
    }
    b->p[b->n++] = (char)c;
    return true;
}

static inline bool jm_buf_put_cp(jm_buf *b, uint32_t cp)
{
    if (cp < 0x80) {
        return jm_buf_put(b, (unsigned char)cp);
    }
    if (cp < 0x800) {
        return jm_buf_put(b, 0xC0 | (cp >> 6)) && jm_buf_put(b, 0x80 | (cp & 0x3F));
    }
    if (cp < 0x10000) {
        return jm_buf_put(b, 0xE0 | (cp >> 12)) && jm_buf_put(b, 0x80 | ((cp >> 6) & 0x3F)) &&
               jm_buf_put(b, 0x80 | (cp & 0x3F));
    }
    return jm_buf_put(b, 0xF0 | (cp >> 18)) && jm_buf_put(b, 0x80 | ((cp >> 12) & 0x3F)) &&
           jm_buf_put(b, 0x80 | ((cp >> 6) & 0x3F)) && jm_buf_put(b, 0x80 | (cp & 0x3F));
}

static inline void jm_ws(jm_parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static inline bool jm_lit(jm_parser *ps, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) {
        return false;
    }
    ps->p += n;
    return true;
}

static inline int jm_hexval(char c)
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

static inline bool jm_hex4(jm_parser *ps, uint32_t *out)
{
    if (ps->end - ps->p < 4) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = jm_hexval(ps->p[i]);
        if (h < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)h;
    }
    ps->p += 4;
    *out = v;
    return true;
}

/* Parses a string whose opening quote is at ps->p. Stores the result in v (type set to string). */
static inline bool jm_string(jm_parser *ps, jm_val *v)
{
    jm_buf b = {nullptr, 0, 0};
    if (ps->p >= ps->end || *ps->p != '"') {
        return false;
    }
    ps->p++;
    for (;;) {
        if (ps->p >= ps->end) {
            goto fail;
        }
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            goto fail;
        }
        if (c != '\\') {
            if (!jm_buf_put(&b, c)) {
                goto fail;
            }
            continue;
        }
        if (ps->p >= ps->end) {
            goto fail;
        }
        c = (unsigned char)*ps->p++;
        switch (c) {
        case '"':
        case '\\':
        case '/':
            if (!jm_buf_put(&b, c)) {
                goto fail;
            }
            break;
        case 'b':
            if (!jm_buf_put(&b, '\b')) {
                goto fail;
            }
            break;
        case 'f':
            if (!jm_buf_put(&b, '\f')) {
                goto fail;
            }
            break;
        case 'n':
            if (!jm_buf_put(&b, '\n')) {
                goto fail;
            }
            break;
        case 'r':
            if (!jm_buf_put(&b, '\r')) {
                goto fail;
            }
            break;
        case 't':
            if (!jm_buf_put(&b, '\t')) {
                goto fail;
            }
            break;
        case 'u': {
            uint32_t cp;
            if (!jm_hex4(ps, &cp)) {
                goto fail;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 && ps->p[0] == '\\' &&
                ps->p[1] == 'u') {
                const char *saved = ps->p;
                ps->p += 2;
                uint32_t lo;
                if (jm_hex4(ps, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    ps->p = saved;
                }
            }
            if (!jm_buf_put_cp(&b, cp)) {
                goto fail;
            }
            break;
        }
        default:
            goto fail;
        }
    }
    if (!jm_buf_put(&b, 0)) {
        goto fail;
    }
    v->type = JM_STRING;
    v->str = b.p;
    v->len = b.n - 1;
    return true;
fail:
    free(b.p);
    return false;
}

static inline bool jm_value(jm_parser *ps, jm_val *v, int depth);

static inline bool jm_push(jm_val *arr, jm_val *item, char *key)
{
    if (arr->count == arr->cap) {
        size_t cap = arr->cap ? arr->cap * 2 : 8;
        jm_val *items = realloc(arr->items, cap * sizeof *items);
        if (items == nullptr) {
            return false;
        }
        arr->items = items;
        char **keys = realloc(arr->keys, cap * sizeof *keys);
        if (keys == nullptr) {
            return false;
        }
        arr->keys = keys;
        arr->cap = cap;
    }
    arr->items[arr->count] = *item;
    arr->keys[arr->count] = key;
    arr->count++;
    return true;
}

static inline bool jm_value(jm_parser *ps, jm_val *v, int depth)
{
    if (depth > 64) {
        return false;
    }
    jm_ws(ps);
    if (ps->p >= ps->end) {
        return false;
    }
    memset(v, 0, sizeof *v);
    char c = *ps->p;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        char close = obj ? '}' : ']';
        ps->p++;
        v->type = obj ? JM_OBJECT : JM_ARRAY;
        jm_ws(ps);
        if (ps->p < ps->end && *ps->p == close) {
            ps->p++;
            return true;
        }
        for (;;) {
            jm_val item;
            char *key = nullptr;
            memset(&item, 0, sizeof item);
            jm_ws(ps);
            if (obj) {
                jm_val k;
                if (!jm_string(ps, &k)) {
                    return false;
                }
                key = k.str;
                jm_ws(ps);
                if (ps->p >= ps->end || *ps->p != ':') {
                    free(key);
                    return false;
                }
                ps->p++;
            }
            /* On failure the item was never pushed, so this function still owns it. */
            if (!jm_value(ps, &item, depth + 1) || !jm_push(v, &item, key)) {
                jm_free(&item);
                free(key);
                return false;
            }
            jm_ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == close) {
                ps->p++;
                return true;
            }
            return false;
        }
    }
    if (c == '"') {
        return jm_string(ps, v);
    }
    if (c == 't' && jm_lit(ps, "true")) {
        v->type = JM_BOOL;
        v->b = true;
        return true;
    }
    if (c == 'f' && jm_lit(ps, "false")) {
        v->type = JM_BOOL;
        v->b = false;
        return true;
    }
    if (c == 'n' && jm_lit(ps, "null")) {
        v->type = JM_NULL;
        return true;
    }
    /* Number. */
    const char *s = ps->p;
    while (ps->p < ps->end && *ps->p != '\0' && strchr("+-0123456789.eE", *ps->p) != nullptr) {
        ps->p++;
    }
    size_t n = (size_t)(ps->p - s);
    if (n == 0 || n > 63) {
        return false;
    }
    char tmp[64];
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    char *e = nullptr;
    v->num = strtod(tmp, &e);
    if (e != tmp + n) {
        return false;
    }
    v->type = JM_NUMBER;
    return true;
}

/* Parses one complete JSON document. On failure, out is left freed and false is returned. */
static inline bool jm_parse(const char *text, size_t n, jm_val *out)
{
    jm_parser ps = {text, text + n};
    memset(out, 0, sizeof *out);
    if (!jm_value(&ps, out, 0)) {
        jm_free(out);
        return false;
    }
    jm_ws(&ps);
    if (ps.p != ps.end) {
        jm_free(out);
        return false;
    }
    return true;
}
