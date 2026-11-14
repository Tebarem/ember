#pragma once
/*
 * A small JSON reader for the vendored WPT data files (test-only, not a module header).
 *
 * Strings are returned as UTF-8 with \u escapes decoded. A lone surrogate becomes U+FFFD, the
 * same as a USVString conversion. Numbers are kept as their source text. The tree is malloc'd
 * and released with json_free, so the leak checkers see no leftovers.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

enum jkind { JK_NULL, JK_BOOL, JK_NUM, JK_STR, JK_ARR, JK_OBJ };

typedef struct jnode {
    int kind;
    bool boolean;
    char *str; /* JK_STR: UTF-8 with a NUL terminator, length in slen; JK_NUM: source text */
    size_t slen;
    size_t count;     /* JK_ARR and JK_OBJ: members */
    struct jnode **vals;
    char **keys;      /* JK_OBJ only */
} jnode;

typedef struct jparser {
    const char *p;
    const char *end;
    bool error;
} jparser;

static jnode *jp_value(jparser *jp);

static void jp_ws(jparser *jp)
{
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) {
        jp->p++;
    }
}

static int jp_hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return -1;
        }
        v = v * 16 + d;
    }
    return v;
}

/* Appends one code point as UTF-8 to a growing buffer. */
static void jb_cp(char **buf, size_t *len, size_t *cap, uint32_t cp)
{
    if (*len + 4 + 1 > *cap) {
        *cap = (*cap + 4) * 2;
        *buf = realloc(*buf, *cap);
    }
    if (cp < 0x80) {
        (*buf)[(*len)++] = (char)cp;
    } else if (cp < 0x800) {
        (*buf)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        (*buf)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        (*buf)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*buf)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
}

/* Parses a string token at jp->p (which is at the opening quote). */
static char *jp_string(jparser *jp, size_t *out_len)
{
    jp->p++;
    size_t cap = 16;
    size_t len = 0;
    char *buf = malloc(cap);
    while (jp->p < jp->end && *jp->p != '"') {
        unsigned char c = (unsigned char)*jp->p;
        if (c == '\\') {
            jp->p++;
            if (jp->p >= jp->end) {
                jp->error = true;
                break;
            }
            char e = *jp->p++;
            switch (e) {
            case '"': jb_cp(&buf, &len, &cap, '"'); break;
            case '\\': jb_cp(&buf, &len, &cap, '\\'); break;
            case '/': jb_cp(&buf, &len, &cap, '/'); break;
            case 'b': jb_cp(&buf, &len, &cap, '\b'); break;
            case 'f': jb_cp(&buf, &len, &cap, '\f'); break;
            case 'n': jb_cp(&buf, &len, &cap, '\n'); break;
            case 'r': jb_cp(&buf, &len, &cap, '\r'); break;
            case 't': jb_cp(&buf, &len, &cap, '\t'); break;
            case 'u': {
                if (jp->end - jp->p < 4) {
                    jp->error = true;
                    break;
                }
                int v = jp_hex4(jp->p);
                jp->p += 4;
                if (v < 0) {
                    jp->error = true;
                    break;
                }
                uint32_t cp = (uint32_t)v;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    int lo = -1;
                    if (jp->end - jp->p >= 6 && jp->p[0] == '\\' && jp->p[1] == 'u') {
                        lo = jp_hex4(jp->p + 2);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
                        jp->p += 6;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                jb_cp(&buf, &len, &cap, cp);
                break;
            }
            default:
                jp->error = true;
                break;
            }
            continue;
        }
        buf[len++] = (char)c;
        if (len + 1 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        jp->p++;
    }
    if (jp->p >= jp->end) {
        jp->error = true;
    } else {
        jp->p++; /* closing quote */
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static jnode *jnode_new(int kind)
{
    jnode *n = calloc(1, sizeof *n);
    n->kind = kind;
    return n;
}

static jnode *jp_value(jparser *jp)
{
    jp_ws(jp);
    if (jp->p >= jp->end) {
        jp->error = true;
        return nullptr;
    }
    char c = *jp->p;
    if (c == '"') {
        jnode *n = jnode_new(JK_STR);
        n->str = jp_string(jp, &n->slen);
        return n;
    }
    if (c == '[') {
        jnode *n = jnode_new(JK_ARR);
        jp->p++;
        jp_ws(jp);
        size_t cap = 0;
        while (!jp->error && jp->p < jp->end && *jp->p != ']') {
            jnode *v = jp_value(jp);
            if (v == nullptr) {
                break;
            }
            if (n->count == cap) {
                cap = cap ? cap * 2 : 8;
                n->vals = realloc(n->vals, cap * sizeof *n->vals);
            }
            n->vals[n->count++] = v;
            jp_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') {
                jp->p++;
            }
        }
        if (jp->p < jp->end) {
            jp->p++;
        }
        return n;
    }
    if (c == '{') {
        jnode *n = jnode_new(JK_OBJ);
        jp->p++;
        jp_ws(jp);
        size_t cap = 0;
        while (!jp->error && jp->p < jp->end && *jp->p != '}') {
            jp_ws(jp);
            if (jp->p >= jp->end || *jp->p != '"') {
                jp->error = true;
                break;
            }
            size_t kl;
            char *key = jp_string(jp, &kl);
            jp_ws(jp);
            if (jp->p >= jp->end || *jp->p != ':') {
                free(key);
                jp->error = true;
                break;
            }
            jp->p++;
            jnode *v = jp_value(jp);
            if (v == nullptr) {
                free(key);
                break;
            }
            if (n->count == cap) {
                cap = cap ? cap * 2 : 8;
                n->vals = realloc(n->vals, cap * sizeof *n->vals);
                n->keys = realloc(n->keys, cap * sizeof *n->keys);
            }
            n->keys[n->count] = key;
            n->vals[n->count++] = v;
            jp_ws(jp);
            if (jp->p < jp->end && *jp->p == ',') {
                jp->p++;
            }
        }
        if (jp->p < jp->end) {
            jp->p++;
        }
        return n;
    }
    if (jp->end - jp->p >= 4 && memcmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        return jnode_new(JK_NULL);
    }
    if (jp->end - jp->p >= 4 && memcmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        jnode *n = jnode_new(JK_BOOL);
        n->boolean = true;
        return n;
    }
    if (jp->end - jp->p >= 5 && memcmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        return jnode_new(JK_BOOL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        jnode *n = jnode_new(JK_NUM);
        const char *s = jp->p;
        while (jp->p < jp->end && ((*jp->p >= '0' && *jp->p <= '9') || *jp->p == '-' || *jp->p == '+' ||
                                   *jp->p == '.' || *jp->p == 'e' || *jp->p == 'E')) {
            jp->p++;
        }
        n->slen = (size_t)(jp->p - s);
        n->str = malloc(n->slen + 1);
        memcpy(n->str, s, n->slen);
        n->str[n->slen] = '\0';
        return n;
    }
    jp->error = true;
    return nullptr;
}

/* Parses a whole document. Returns nullptr on a syntax error. */
static jnode *json_parse(const char *text, size_t n)
{
    jparser jp = {text, text + n, false};
    /* Skip a UTF-8 BOM, if any. */
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        jp.p += 3;
    }
    jnode *root = jp_value(&jp);
    if (jp.error) {
        return nullptr;
    }
    return root;
}

static void json_free(jnode *n)
{
    if (n == nullptr) {
        return;
    }
    for (size_t i = 0; i < n->count; i++) {
        json_free(n->vals[i]);
        if (n->keys != nullptr) {
            free(n->keys[i]);
        }
    }
    free(n->vals);
    free(n->keys);
    free(n->str);
    free(n);
}

/* Object member lookup. nullptr when absent or when n is not an object. */
static const jnode *json_get(const jnode *n, const char *key)
{
    if (n == nullptr || n->kind != JK_OBJ) {
        return nullptr;
    }
    for (size_t i = 0; i < n->count; i++) {
        if (strcmp(n->keys[i], key) == 0) {
            return n->vals[i];
        }
    }
    return nullptr;
}

static bool json_is_str(const jnode *n)
{
    return n != nullptr && n->kind == JK_STR;
}

/* Reads a file the test data lives in and parses it. Caller frees with json_free. */
static jnode *json_load_file(const char *path)
{
    size_t size = 0;
    char *text = SDL_LoadFile(path, &size);
    if (text == nullptr) {
        return nullptr;
    }
    jnode *root = json_parse(text, size);
    SDL_free(text);
    return root;
}
