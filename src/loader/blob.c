/*
 * Blob core (W7c). See blob.h.
 */
#include "loader/blob.h"

#include <string.h>

static void blob_unref_data(embi_blob *b)
{
    if (b->data != nullptr) {
        embi_free(b->data, b->alloc);
        b->data = nullptr;
        b->alloc = 0;
    }
}

void embi_blob_type_normalize(embi_str in, embi_sb *out)
{
    for (uint32_t i = 0; i < in.len; i++) {
        unsigned char c = (unsigned char)in.ptr[i];
        if (c < 0x20 || c > 0x7e) {
            embi_sb_clear(out);
            return;
        }
    }
    for (uint32_t i = 0; i < in.len; i++) {
        char c = in.ptr[i];
        embi_sb_append_char(out, (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
    }
}

static embi_blob *blob_make(void)
{
    embi_blob *b = embi_alloc(sizeof *b);
    *b = (embi_blob){0};
    b->refs = 1;
    return b;
}

embi_blob *embi_blob_new(const embi_blob_part *parts, uint32_t nparts, embi_str type)
{
    size_t total = 0;
    for (uint32_t i = 0; i < nparts; i++) {
        total += parts[i].blob != nullptr ? parts[i].blob->size : parts[i].len;
    }
    embi_blob *b = blob_make();
    if (total > 0) {
        b->data = embi_alloc(total);
        b->alloc = total;
        size_t pos = 0;
        for (uint32_t i = 0; i < nparts; i++) {
            const uint8_t *src = nullptr;
            size_t n = 0;
            if (parts[i].blob != nullptr) {
                src = embi_blob_bytes(parts[i].blob, &n);
            } else {
                src = parts[i].bytes;
                n = parts[i].len;
            }
            if (n > 0) {
                memcpy(b->data + pos, src, n);
                pos += n;
            }
        }
    }
    b->size = total;
    embi_blob_type_normalize(type, &b->type);
    return b;
}

/* The spec's relative index: negative counts from the end, and the result is clamped to [0, size]. */
static size_t relative_index(int64_t v, size_t size)
{
    if (v < 0) {
        int64_t r = (int64_t)size + v;
        return r < 0 ? 0 : (size_t)r;
    }
    return (uint64_t)v > size ? size : (size_t)v;
}

embi_blob *embi_blob_slice(const embi_blob *b, int64_t start, int64_t end, embi_str type)
{
    size_t rs = relative_index(start, b->size);
    size_t re = relative_index(end, b->size);
    size_t span = re > rs ? re - rs : 0;
    /* The bytes belong to the root blob; a slice of a slice retains the same root. */
    embi_blob *root = (embi_blob *)(b->root != nullptr ? b->root : b);
    embi_blob *s = blob_make();
    s->root = embi_blob_retain(root);
    s->data = root->data;
    s->alloc = 0;
    s->off = b->off + rs;
    s->size = span;
    embi_blob_type_normalize(type, &s->type);
    return s;
}

embi_blob *embi_blob_retain(embi_blob *b)
{
    b->refs++;
    return b;
}

void embi_blob_release(embi_blob *b)
{
    if (b == nullptr) {
        return;
    }
    if (--b->refs > 0) {
        return;
    }
    if (b->root != nullptr) {
        embi_blob_release(b->root);
    } else {
        blob_unref_data(b);
    }
    embi_sb_free(&b->type);
    embi_free(b, sizeof *b);
}

const uint8_t *embi_blob_bytes(const embi_blob *b, size_t *len)
{
    *len = b->size;
    if (b->size == 0 || b->data == nullptr) {
        return nullptr;
    }
    return b->data + b->off;
}

/* ------------------------------------------------------------------ blob: URLs */

void embi_blob_url_create(embi_blob_urls *reg, embi_rng *rng, embi_str origin, embi_blob *b, embi_sb *out)
{
    char uuid[37];
    embi_rng_uuid(rng, uuid);
    embi_blob_url_entry *e = nullptr;
    embi_vec_reserve(&reg->entries, reg->entries.len + 1u);
    e = &reg->entries.data[reg->entries.len++];
    e->url = (embi_sb){nullptr, 0, 0};
    embi_sb_append(&e->url, EMBI_STR("blob:"));
    embi_sb_append(&e->url, origin);
    embi_sb_append(&e->url, EMBI_STR("/"));
    embi_sb_append(&e->url, (embi_str){uuid, 36});
    e->blob = embi_blob_retain(b);
    embi_sb_append(out, (embi_str){e->url.data, e->url.len});
}

/* The URL without its fragment. */
static embi_str strip_fragment(embi_str url)
{
    for (uint32_t i = 0; i < url.len; i++) {
        if (url.ptr[i] == '#') {
            return (embi_str){url.ptr, i};
        }
    }
    return url;
}

embi_blob *embi_blob_url_resolve(const embi_blob_urls *reg, embi_str url)
{
    embi_str key = strip_fragment(url);
    for (uint32_t i = 0; i < reg->entries.len; i++) {
        embi_blob_url_entry *e = &reg->entries.data[i];
        if (embi_str_eq((embi_str){e->url.data, e->url.len}, key)) {
            return e->blob;
        }
    }
    return nullptr;
}

void embi_blob_url_revoke(embi_blob_urls *reg, embi_str url)
{
    embi_str key = strip_fragment(url);
    for (uint32_t i = 0; i < reg->entries.len; i++) {
        embi_blob_url_entry *e = &reg->entries.data[i];
        if (embi_str_eq((embi_str){e->url.data, e->url.len}, key)) {
            embi_sb_free(&e->url);
            embi_blob_release(e->blob);
            for (uint32_t k = i + 1; k < reg->entries.len; k++) {
                reg->entries.data[k - 1] = reg->entries.data[k];
            }
            reg->entries.len--;
            return;
        }
    }
}

void embi_blob_urls_free(embi_blob_urls *reg)
{
    for (uint32_t i = 0; i < reg->entries.len; i++) {
        embi_sb_free(&reg->entries.data[i].url);
        embi_blob_release(reg->entries.data[i].blob);
    }
    embi_free(reg->entries.data, (size_t)reg->entries.cap * sizeof reg->entries.data[0]);
    *reg = (embi_blob_urls){0};
}
