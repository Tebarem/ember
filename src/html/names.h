#pragma once
/*
 * Tag and attribute name atoms for the HTML parser. The tokenizer and the tree builder look names
 * up through a per-thread cache keyed by the name bytes, so the atom table (and its lock) is only
 * used the first time a thread sees a name. Atom ids are stable until embi_atom_shutdown,
 * which can rebuild the table with different ids: call embi_html_name_cache_reset on every thread
 * that parsed (like embi_sel_lower_cache_reset) after a shutdown.
 *
 * Names must already be lower case where the spec lowercases them: the cache is exact-match.
 */
#include "base/base.h"

#include <string.h>

typedef struct embi_html_name_entry {
    uint64_t w0;
    uint64_t w1;
    embi_atom atom; /* 0 = empty slot */
    uint32_t len;
} embi_html_name_entry;

#define EMBI_HTML_NAME_SLOTS 256u
extern thread_local embi_html_name_entry embi_html_name_cache[EMBI_HTML_NAME_SLOTS];

/* Clears the calling thread's name cache. Required after embi_atom_shutdown. */
void embi_html_name_cache_reset(void);

/* Atom for a name that is not 1..16 bytes, or on a cache miss. create == false looks the name up
 * without interning (0 when it was never interned). */
embi_atom embi_html_name_atom_slow(const char *p, size_t n, bool create);

/* The 16-byte key of a name: two overlapping 8-byte loads (or less for short names). It maps
 * different names of equal length to different keys. n is 1..16. */
[[gnu::always_inline]] static inline void embi_html_name_key(const char *p, size_t n, uint64_t *w0,
                                                             uint64_t *w1)
{
    uint64_t a = 0;
    uint64_t b = 0;
    if (n >= 8) {
        memcpy(&a, p, 8);
        memcpy(&b, p + n - 8, 8);
    } else if (n >= 4) {
        uint32_t x;
        uint32_t y;
        memcpy(&x, p, 4);
        memcpy(&y, p + n - 4, 4);
        a = (uint64_t)x | ((uint64_t)y << 32);
    } else {
        a = (uint64_t)(unsigned char)p[0] | ((uint64_t)(unsigned char)p[n >> 1] << 8) |
            ((uint64_t)(unsigned char)p[n - 1] << 16);
    }
    *w0 = a;
    *w1 = b;
}

[[gnu::always_inline]] static inline embi_atom embi_html_name_atom(const char *p, size_t n,
                                                                   bool create)
{
    if (n == 0 || n > 16) {
        return embi_html_name_atom_slow(p, n, create);
    }
    uint64_t a;
    uint64_t b;
    embi_html_name_key(p, n, &a, &b);
    uint64_t h = (a * 0x9E3779B97F4A7C15ull) ^ (b * 0xC2B2AE3D27D4EB4Full) ^ n;
    h *= 0xFF51AFD7ED558CCDull;
    uint32_t slot = (uint32_t)(h >> 56) & (EMBI_HTML_NAME_SLOTS - 1u);
    embi_html_name_entry *e0 = &embi_html_name_cache[slot];
    if (e0->w0 == a && e0->w1 == b && e0->len == n && e0->atom) {
        return e0->atom;
    }
    embi_html_name_entry *e1 = &embi_html_name_cache[slot ^ 1u];
    if (e1->w0 == a && e1->w1 == b && e1->len == n && e1->atom) {
        return e1->atom;
    }
    return embi_html_name_atom_slow(p, n, create);
}
