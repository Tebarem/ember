#include "codec/codec.h"

#include <SDL3/SDL_stdinc.h>

void embi_buf_free(embi_buf *b)
{
    if (b->data) {
        embi_free(b->data, b->cap);
    }
    *b = (embi_buf){0};
}

bool embi_buf_reserve(embi_buf *b, size_t extra)
{
    if (extra > SIZE_MAX - b->len) {
        return false;
    }
    size_t need = b->len + extra;
    if (need <= b->cap) {
        return true;
    }
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        cap = cap > SIZE_MAX / 2 ? need : cap * 2;
    }
    b->data = embi_realloc(b->data, b->cap, cap);
    b->cap = cap;
    return true;
}

bool embi_buf_append(embi_buf *b, const void *src, size_t n)
{
    if (!embi_buf_reserve(b, n)) {
        return false;
    }
    if (n) {
        SDL_memcpy(b->data + b->len, src, n);
    }
    b->len += n;
    return true;
}
