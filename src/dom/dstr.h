#pragma once
/*
 * DOM strings. Same representation as JS flat strings: Latin-1 (one byte per code unit) when every
 * code unit is <= 0xFF, otherwise UTF-16 code units. The representation is canonical: a string is
 * `wide` only if at least one unit is > 0xFF. Equal strings therefore have equal representations
 * and equal hashes.
 *
 * An embi_dstr is a non-owning view. Strings made by the embi_dstr_from_* and embi_dstr_copy
 * functions and by embi_dbuf_finish live in a slab and must be released with embi_dstr_free. An
 * empty string has ptr nullptr. Callers must not dereference it.
 */
#include "base/base.h"
#include "dom/slab.h"

typedef struct embi_dstr {
    const void *ptr; /* uint8_t[len] when !wide, uint16_t[len] when wide */
    uint32_t len;    /* code units */
    bool wide;
} embi_dstr;

static inline embi_dstr embi_dstr_empty(void)
{
    return (embi_dstr){nullptr, 0, false};
}

/* Copies n bytes. Up to 16 bytes (names, short text and values) are two overlapping constant-size
 * copies, which is much cheaper than a call into the C runtime for each small string. */
static inline void embi_copy_bytes(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (n > 16) {
        SDL_memcpy(d, s, n);
        return;
    }
    if (n >= 8) {
        SDL_memcpy(d, s, 8);
        SDL_memcpy(d + n - 8, s + n - 8, 8);
    } else if (n >= 4) {
        SDL_memcpy(d, s, 4);
        SDL_memcpy(d + n - 4, s + n - 4, 4);
    } else if (n > 0) {
        d[0] = s[0];
        d[n - 1] = s[n - 1];
        if (n == 3) {
            d[1] = s[1];
        }
    }
}

/* True when every one of the n bytes is below 0x80. */
static inline bool embi_bytes_ascii(const void *p, size_t n)
{
    const unsigned char *s = p;
    size_t k = 0;
    for (; k + 8 <= n; k += 8) {
        uint64_t v;
        SDL_memcpy(&v, s + k, 8);
        if ((v & 0x8080808080808080ull) != 0) {
            return false;
        }
    }
    for (; k < n; k++) {
        if (s[k] >= 0x80) {
            return false;
        }
    }
    return true;
}

/* UTF-8 text as DOM string units: the number of UTF-16 code units, and in *wide whether any unit
 * is above 0xFF. embi_utf8_write stores them: bytes when wide is false, else uint16_t (wide is the
 * width of the target, which may be wider than the text needs). dst must hold the measured
 * length. Invalid UTF-8 decodes to U+FFFD as in embi_utf8_decode. */
uint32_t embi_utf8_units(embi_str s, bool *wide);
void embi_utf8_write(void *dst, bool wide, embi_str s);

static inline uint16_t embi_dstr_unit(embi_dstr d, uint32_t i)
{
    return d.wide ? ((const uint16_t *)d.ptr)[i] : ((const uint8_t *)d.ptr)[i];
}

/* Constructors allocate from `s` and copy their input. */
embi_dstr embi_dstr_from_utf8(embi_slab *s, embi_str text);
embi_dstr embi_dstr_from_units(embi_slab *s, const uint16_t *units, uint32_t n);
embi_dstr embi_dstr_from_latin1(embi_slab *s, const uint8_t *bytes, uint32_t n);
embi_dstr embi_dstr_copy(embi_slab *s, embi_dstr d);
/* Frees the storage of a string made by a constructor above. Ignores an empty string. */
void embi_dstr_free(embi_dstr d);

bool embi_dstr_eq(embi_dstr a, embi_dstr b);
/* Code unit order. Negative, zero or positive. */
int embi_dstr_cmp(embi_dstr a, embi_dstr b);
/* Exact match against UTF-8 text. Invalid UTF-8 decodes to U+FFFD per embi_utf8_decode. */
bool embi_dstr_eq_utf8(embi_dstr a, embi_str text);
/* ASCII case-insensitive match against UTF-8 text. Only A-Z fold. */
bool embi_dstr_ieq_utf8(embi_dstr a, embi_str text);
uint64_t embi_dstr_hash(embi_dstr d);
/* Writes the UTF-8 encoding. Lone surrogates become U+FFFD. Returns the bytes needed; writes at
 * most cap bytes. */
size_t embi_dstr_to_utf8(embi_dstr d, char *out, size_t cap);
void embi_dstr_append_utf8(embi_sb *sb, embi_dstr d);
/* Interns the string as an atom (its UTF-8 bytes). */
embi_atom embi_dstr_atom(embi_dstr d);

/* Builder. Accumulates UTF-16 units and narrows on finish when every unit fits in a byte. Call
 * embi_dbuf_init before use. */
typedef struct embi_dbuf {
    embi_slab *slab;
    EMBI_VEC(uint16_t) units;
} embi_dbuf;

void embi_dbuf_init(embi_dbuf *b, embi_slab *s);
/* Frees the scratch storage without producing a string. */
void embi_dbuf_free(embi_dbuf *b);
void embi_dbuf_append(embi_dbuf *b, embi_dstr d);
void embi_dbuf_append_utf8(embi_dbuf *b, embi_str text);
void embi_dbuf_append_unit(embi_dbuf *b, uint16_t unit);
void embi_dbuf_append_cp(embi_dbuf *b, uint32_t cp);
uint32_t embi_dbuf_len(const embi_dbuf *b);
/* Returns the string in the builder's slab and resets the builder to empty (still usable). */
embi_dstr embi_dbuf_finish(embi_dbuf *b);
