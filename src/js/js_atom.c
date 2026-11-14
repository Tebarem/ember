/*
 * Atom table: one interned string per content per runtime. Open addressing with linear probing,
 * keyed by the cached string hash. Atoms are permanent in J1, so the collector marks every atom
 * as a root (see gc.c).
 */
#include "js/runtime.h"
#include "js/string.h"

#define EMBI_ATOMS_INITIAL_CAP 256u

void embi_js_atoms_init(embi_js_atoms *a)
{
    a->cap = EMBI_ATOMS_INITIAL_CAP;
    a->count = 0;
    a->slots = embi_calloc(a->cap, sizeof *a->slots);
}

void embi_js_atoms_free(embi_js_atoms *a)
{
    embi_free(a->slots, (size_t)a->cap * sizeof *a->slots);
    memset(a, 0, sizeof *a);
}

static void slots_place(embi_js_string **slots, uint32_t cap, embi_js_string *s, uint32_t hash)
{
    uint32_t i = hash & (cap - 1u);
    while (slots[i] != nullptr) {
        i = (i + 1u) & (cap - 1u);
    }
    slots[i] = s;
}

static void atoms_grow(embi_js_atoms *a)
{
    uint32_t ncap = a->cap * 2u;
    embi_js_string **ns = embi_calloc(ncap, sizeof *ns);
    for (uint32_t i = 0; i < a->cap; i++) {
        if (a->slots[i] != nullptr) {
            slots_place(ns, ncap, a->slots[i], a->slots[i]->hash);
        }
    }
    embi_free(a->slots, (size_t)a->cap * sizeof *a->slots);
    a->slots = ns;
    a->cap = ncap;
}

embi_js_string *embi_js_atom_intern(embi_js_runtime *rt, embi_js_string *s)
{
    if (embi_js_str_is_atom(s)) {
        return s;
    }
    /* Atoms are always flat: a rope is flattened first (it may allocate). */
    if (embi_js_str_is_rope(s)) {
        s = embi_js_str_flat(rt, embi_js_from_string(s));
        if (embi_js_str_is_atom(s)) {
            return s;
        }
    }
    embi_js_atoms *a = &rt->atoms;
    uint32_t hash = embi_js_str_hash(s);
    uint32_t mask = a->cap - 1u;
    for (uint32_t i = hash & mask; a->slots[i] != nullptr; i = (i + 1u) & mask) {
        embi_js_string *e = a->slots[i];
        if (e->hash == hash && embi_js_str_equal(e, s)) {
            return e;
        }
    }

    /* Keep the load factor at or below one half. */
    if ((a->count + 1u) * 2u > a->cap) {
        atoms_grow(a);
    }
    s->hdr.flags |= EMBI_JS_FLAG_ATOM;
    slots_place(a->slots, a->cap, s, hash);
    a->count++;
    return s;
}

embi_js_string *embi_js_atom_intern_utf8(embi_js_runtime *rt, const char *p, size_t len)
{
    /* FP1a: an ASCII name that is already interned is found without building a string (the
     * compiler interns every identifier occurrence, and each used to allocate a throwaway cell).
     * The hash is the one embi_js_str_hash computes for the same Latin-1 content. This (and the
     * parser's na_atom cache) relies on atoms being permanent: an atom found now is still the atom
     * later, and the collector never frees it. */
    if (len <= EMBI_JS_STR_MAX_LEN) {
        uint32_t h = EMBI_JS_STR_HASH_SEED;
        size_t i = 0;
        for (; i < len; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c >= 0x80u) {
                break;
            }
            h = (h ^ c) * EMBI_JS_STR_HASH_PRIME;
        }
        if (i == len) {
            h = embi_js_str_hash_finish(h);
            const embi_js_atoms *a = &rt->atoms;
            uint32_t mask = a->cap - 1u;
            for (uint32_t k = h & mask; a->slots[k] != nullptr; k = (k + 1u) & mask) {
                const embi_js_string *e = a->slots[k];
                if (e->hash == h && embi_js_str_len(e) == len && !embi_js_str_is_wide(e) &&
                    memcmp(embi_js_str_latin1(e), p, len) == 0) {
                    return a->slots[k];
                }
            }
        }
    }
    embi_js_string *s = embi_js_str_new_utf8(rt, p, len);
    return embi_js_atom_intern(rt, s);
}

uint32_t embi_js_atoms_count(const embi_js_runtime *rt)
{
    return rt->atoms.count;
}
