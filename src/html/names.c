#include "html/names.h"

thread_local embi_html_name_entry embi_html_name_cache[EMBI_HTML_NAME_SLOTS];

void embi_html_name_cache_reset(void)
{
    memset(embi_html_name_cache, 0, sizeof embi_html_name_cache);
}

embi_atom embi_html_name_atom_slow(const char *p, size_t n, bool create)
{
    embi_atom a = create ? embi_atom_intern(p, (uint32_t)n) : embi_atom_find(p, (uint32_t)n);
    if (a == 0 || n == 0 || n > 16) {
        return a;
    }
    uint64_t w0;
    uint64_t w1;
    embi_html_name_key(p, n, &w0, &w1);
    uint64_t h = (w0 * 0x9E3779B97F4A7C15ull) ^ (w1 * 0xC2B2AE3D27D4EB4Full) ^ n;
    h *= 0xFF51AFD7ED558CCDull;
    uint32_t slot = (uint32_t)(h >> 56) & (EMBI_HTML_NAME_SLOTS - 1u);
    embi_html_name_entry *e0 = &embi_html_name_cache[slot];
    embi_html_name_entry *e1 = &embi_html_name_cache[slot ^ 1u];
    /* An empty entry first; otherwise replace the second one, so the first keeps a name that
     * was filled earlier. */
    embi_html_name_entry *e = e0->atom == 0 ? e0 : e1;
    e->w0 = w0;
    e->w1 = w1;
    e->len = (uint32_t)n;
    e->atom = a;
    return a;
}
