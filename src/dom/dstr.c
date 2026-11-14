#include "dom/dstr.h"

/* Builds a canonical string from UTF-16 units. */
static embi_dstr alloc_string(embi_slab *s, const uint16_t *units, uint32_t n)
{
    if (n == 0) {
        return embi_dstr_empty();
    }
    bool wide = false;
    for (uint32_t i = 0; i < n && !wide; i++) {
        wide = units[i] > 0xFFu;
    }
    embi_dstr d = {nullptr, n, wide};
    if (wide) {
        uint16_t *p = embi_slab_alloc(s, (size_t)n * 2u);
        for (uint32_t i = 0; i < n; i++) {
            p[i] = units[i];
        }
        d.ptr = p;
    } else {
        uint8_t *p = embi_slab_alloc(s, n);
        for (uint32_t i = 0; i < n; i++) {
            p[i] = (uint8_t)units[i];
        }
        d.ptr = p;
    }
    return d;
}

embi_dstr embi_dstr_from_units(embi_slab *s, const uint16_t *units, uint32_t n)
{
    return alloc_string(s, units, n);
}

embi_dstr embi_dstr_from_latin1(embi_slab *s, const uint8_t *bytes, uint32_t n)
{
    if (n == 0) {
        return embi_dstr_empty();
    }
    uint8_t *p = embi_slab_alloc(s, n);
    SDL_memcpy(p, bytes, n);
    return (embi_dstr){p, n, false};
}

uint32_t embi_utf8_units(embi_str s, bool *wide)
{
    if (embi_bytes_ascii(s.ptr, s.len)) {
        *wide = false;
        return s.len;
    }
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    uint32_t n = 0;
    bool w = false;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        n += cp > 0xFFFFu ? 2u : 1u;
        w = w || cp > 0xFFu;
    }
    *wide = w;
    return n;
}

void embi_utf8_write(void *dst, bool wide, embi_str s)
{
    if (!wide && embi_bytes_ascii(s.ptr, s.len)) {
        embi_copy_bytes(dst, s.ptr, s.len);
        return;
    }
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    uint8_t *d8 = dst;
    uint16_t *d16 = dst;
    uint32_t i = 0;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (cp > 0xFFFFu) {
            cp -= 0x10000u;
            d16[i++] = (uint16_t)(0xD800u + (cp >> 10));
            d16[i++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
        } else if (wide) {
            d16[i++] = (uint16_t)cp;
        } else {
            d8[i++] = (uint8_t)cp;
        }
    }
}

embi_dstr embi_dstr_from_utf8(embi_slab *sl, embi_str text)
{
    bool wide;
    uint32_t n = embi_utf8_units(text, &wide);
    if (n == 0) {
        return embi_dstr_empty();
    }
    void *p = embi_slab_alloc(sl, (size_t)n * (wide ? 2u : 1u));
    embi_utf8_write(p, wide, text);
    return (embi_dstr){p, n, wide};
}

embi_dstr embi_dstr_copy(embi_slab *s, embi_dstr d)
{
    if (d.len == 0) {
        return embi_dstr_empty();
    }
    if (d.wide) {
        uint16_t *p = embi_slab_alloc(s, (size_t)d.len * 2u);
        embi_copy_bytes(p, d.ptr, (size_t)d.len * 2u);
        return (embi_dstr){p, d.len, true};
    }
    uint8_t *p = embi_slab_alloc(s, d.len);
    embi_copy_bytes(p, d.ptr, d.len);
    return (embi_dstr){p, d.len, false};
}

void embi_dstr_free(embi_dstr d)
{
    if (d.len != 0) {
        embi_slab_free((void *)d.ptr);
    }
}

bool embi_dstr_eq(embi_dstr a, embi_dstr b)
{
    if (a.len != b.len) {
        return false;
    }
    if (a.len == 0) {
        return true;
    }
    if (a.wide == b.wide) {
        size_t bytes = (size_t)a.len * (a.wide ? 2u : 1u);
        return SDL_memcmp(a.ptr, b.ptr, bytes) == 0;
    }
    for (uint32_t i = 0; i < a.len; i++) {
        if (embi_dstr_unit(a, i) != embi_dstr_unit(b, i)) {
            return false;
        }
    }
    return true;
}

int embi_dstr_cmp(embi_dstr a, embi_dstr b)
{
    uint32_t n = a.len < b.len ? a.len : b.len;
    for (uint32_t i = 0; i < n; i++) {
        int ua = embi_dstr_unit(a, i);
        int ub = embi_dstr_unit(b, i);
        if (ua != ub) {
            return ua - ub;
        }
    }
    return (int)a.len - (int)b.len;
}

/* Reads the next code point at *i, combining surrogate pairs. Lone surrogates yield U+FFFD. */
static uint32_t next_cp(embi_dstr d, uint32_t *i)
{
    uint32_t u = embi_dstr_unit(d, *i);
    (*i)++;
    if (u >= 0xD800u && u <= 0xDBFFu && *i < d.len) {
        uint32_t lo = embi_dstr_unit(d, *i);
        if (lo >= 0xDC00u && lo <= 0xDFFFu) {
            (*i)++;
            return 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
        }
    }
    if (u >= 0xD800u && u <= 0xDFFFu) {
        return 0xFFFDu;
    }
    return u;
}

bool embi_dstr_eq_utf8(embi_dstr a, embi_str text)
{
    const char *p = text.ptr;
    const char *end = text.ptr + text.len;
    uint32_t i = 0;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (i >= a.len || next_cp(a, &i) != cp) {
            return false;
        }
    }
    return i == a.len;
}

static uint32_t ascii_lower(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32u : c;
}

bool embi_dstr_ieq_utf8(embi_dstr a, embi_str text)
{
    const char *p = text.ptr;
    const char *end = text.ptr + text.len;
    uint32_t i = 0;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (i >= a.len || ascii_lower(next_cp(a, &i)) != ascii_lower(cp)) {
            return false;
        }
    }
    return i == a.len;
}

uint64_t embi_dstr_hash(embi_dstr d)
{
    if (d.len == 0) {
        return embi_hash_bytes(nullptr, 0, 0);
    }
    if (d.wide) {
        return embi_hash_bytes(d.ptr, (size_t)d.len * 2u, 1);
    }
    return embi_hash_bytes(d.ptr, d.len, 0);
}

void embi_dstr_append_utf8(embi_sb *sb, embi_dstr d)
{
    uint32_t i = 0;
    while (i < d.len) {
        embi_sb_append_cp(sb, next_cp(d, &i));
    }
}

size_t embi_dstr_to_utf8(embi_dstr d, char *out, size_t cap)
{
    size_t need = 0;
    char tmp[4];
    uint32_t i = 0;
    while (i < d.len) {
        size_t n = embi_utf8_encode(next_cp(d, &i), tmp);
        for (size_t k = 0; k < n; k++) {
            if (need + k < cap) {
                out[need + k] = tmp[k];
            }
        }
        need += n;
    }
    return need;
}

embi_atom embi_dstr_atom(embi_dstr d)
{
    /* ASCII narrow text is its own UTF-8, so it is interned in place (no copy). Ids are very
     * common, so this avoids a buffer allocation and a free per id. */
    if (!d.wide) {
        const unsigned char *p = d.ptr;
        bool ascii = true;
        for (uint32_t i = 0; i < d.len; i++) {
            if (p[i] >= 0x80) {
                ascii = false;
                break;
            }
        }
        if (ascii) {
            return embi_atom_intern(d.len ? (const char *)p : "", d.len);
        }
    }
    embi_sb sb = {nullptr, 0, 0};
    embi_dstr_append_utf8(&sb, d);
    embi_atom id = embi_atom_intern(sb.data ? sb.data : "", sb.len);
    embi_sb_free(&sb);
    return id;
}

void embi_dbuf_init(embi_dbuf *b, embi_slab *s)
{
    b->slab = s;
    b->units.data = nullptr;
    b->units.len = 0;
    b->units.cap = 0;
}

void embi_dbuf_free(embi_dbuf *b)
{
    embi_vec_free(&b->units);
}

void embi_dbuf_append_unit(embi_dbuf *b, uint16_t unit)
{
    embi_vec_push(&b->units, unit);
}

void embi_dbuf_append(embi_dbuf *b, embi_dstr d)
{
    if (d.len == 0) {
        return;
    }
    embi_vec_reserve(&b->units, b->units.len + d.len);
    for (uint32_t i = 0; i < d.len; i++) {
        b->units.data[b->units.len++] = embi_dstr_unit(d, i);
    }
}

void embi_dbuf_append_cp(embi_dbuf *b, uint32_t cp)
{
    if (cp > 0xFFFFu) {
        cp -= 0x10000u;
        embi_dbuf_append_unit(b, (uint16_t)(0xD800u + (cp >> 10)));
        embi_dbuf_append_unit(b, (uint16_t)(0xDC00u + (cp & 0x3FFu)));
    } else {
        embi_dbuf_append_unit(b, (uint16_t)cp);
    }
}

void embi_dbuf_append_utf8(embi_dbuf *b, embi_str text)
{
    const char *p = text.ptr;
    const char *end = text.ptr + text.len;
    while (p < end) {
        embi_dbuf_append_cp(b, embi_utf8_decode(&p, end));
    }
}

uint32_t embi_dbuf_len(const embi_dbuf *b)
{
    return b->units.len;
}

embi_dstr embi_dbuf_finish(embi_dbuf *b)
{
    embi_dstr d = alloc_string(b->slab, b->units.data, b->units.len);
    b->units.len = 0;
    return d;
}
