/*
 * Ancestor bloom filter for selector matching (W2b). A counting filter over 256 buckets: an element
 * adds its tag (exact and lowercase), id and class buckets when the walk enters it, and removes them
 * when it leaves. Counts saturate at 255 and then stay there, so the filter can give false positives
 * but never false negatives.
 */
#include "css/selector.h"

#include <string.h>

uint32_t embi_bloom_hash_atom(embi_atom a)
{
    return (uint32_t)(embi_hash64((uint64_t)a) & (EMBI_BLOOM_SIZE - 1));
}

void embi_bloom_clear(embi_bloom *b)
{
    memset(b->count, 0, sizeof b->count);
    embi_sel_lower_cache_reset();
}

static void bump(embi_bloom *b, uint32_t bucket, bool add)
{
    uint8_t *c = &b->count[bucket];
    if (*c == 255) {
        return;
    }
    if (add) {
        (*c)++;
    } else if (*c > 0) {
        (*c)--;
    }
}

static void apply(embi_bloom *b, embi_element *el, bool add)
{
    embi_atom name = el->base.name;
    bump(b, embi_bloom_hash_atom(name), add);
    embi_atom lower = embi_sel_lower_atom(name);
    if (lower != name) {
        bump(b, embi_bloom_hash_atom(lower), add);
    }
    embi_atom id = embi_el_id(el);
    if (id != 0) {
        bump(b, embi_bloom_hash_atom(id), add);
    }
    uint32_t n = 0;
    const embi_atom *cls = embi_el_classes(el, &n);
    for (uint32_t k = 0; k < n; k++) {
        bump(b, embi_bloom_hash_atom(cls[k]), add);
    }
}

void embi_bloom_push(embi_bloom *b, embi_element *el)
{
    apply(b, el, true);
}

void embi_bloom_pop(embi_bloom *b, embi_element *el)
{
    apply(b, el, false);
}

bool embi_bloom_test(const embi_bloom *b, uint32_t bucket)
{
    return b->count[bucket & (EMBI_BLOOM_SIZE - 1)] != 0;
}
