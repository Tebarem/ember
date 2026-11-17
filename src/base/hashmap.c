#include "base/base.h"

/* Mixing constants (arbitrary odd values). */
#define EMBI_HASH_K0 0xa0761d6478bd642full
#define EMBI_HASH_K1 0xe7037ed1a0b428dbull
#define EMBI_HASH_K2 0x8ebc6af09c88c6e3ull
#define EMBI_HASH_K3 0x589965cc75374cc3ull

#define EMBI_MAP_MIN_CAP 16u
#define EMBI_MAP_LOAD_PCT 85u

/* 64x64 to 128-bit multiply, folded to 64 bits (the wyhash mum step). The portable form below gives the same value as the
 * compiler's 128-bit product (the hash values, and so the iteration order of the maps, do not depend on which is used). */
static uint64_t mum(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    unsigned __int128 p = (unsigned __int128)a * b;
    return (uint64_t)p ^ (uint64_t)(p >> 64);
#else
    uint64_t ha = a >> 32;
    uint64_t la = (uint32_t)a;
    uint64_t hb = b >> 32;
    uint64_t lb = (uint32_t)b;
    uint64_t rh = ha * hb;
    uint64_t rm0 = ha * lb;
    uint64_t rm1 = hb * la;
    uint64_t rl = la * lb;
    uint64_t t = rl + (rm0 << 32);
    uint64_t c = t < rl;
    uint64_t lo = t + (rm1 << 32);
    c += lo < t;
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    return lo ^ hi;
#endif
}

uint64_t embi_hash64(uint64_t x)
{
    return mum(mum(x ^ EMBI_HASH_K0, EMBI_HASH_K1) ^ EMBI_HASH_K2, x ^ EMBI_HASH_K3);
}

uint64_t embi_hash_sampled(const void *p, size_t n, uint64_t seed)
{
    if (n <= 2048u) {
        return embi_hash_bytes(p, n, seed); /* (past this the samples below stay between the two ends) */
    }
    const unsigned char *s = p;
    uint64_t h = embi_hash_bytes(s, 512u, seed ^ (uint64_t)n);
    h = embi_hash_bytes(s + n - 512u, 512u, h);
    const size_t span = n - 1024u - 8u, step = span / 64u;
    uint64_t w[64];
    for (size_t i = 0; i < 64u; i++) {
        memcpy(&w[i], s + 512u + i * step, 8);
    }
    return embi_hash_bytes(w, sizeof w, h);
}

uint64_t embi_hash_bytes(const void *p, size_t n, uint64_t seed)
{
    const unsigned char *s = p;
    uint64_t h = mum(seed ^ EMBI_HASH_K0, EMBI_HASH_K1) ^ (uint64_t)n;
    while (n >= 8) {
        uint64_t w;
        SDL_memcpy(&w, s, 8);
        h = mum(h ^ w, EMBI_HASH_K2);
        s += 8;
        n -= 8;
    }
    uint64_t tail = 0;
    if (n) {
        SDL_memcpy(&tail, s, n);
    }
    h = mum(h ^ tail, EMBI_HASH_K3);
    return mum(h ^ EMBI_HASH_K2, EMBI_HASH_K1 ^ EMBI_HASH_K3);
}

void embi_hashmap_init(embi_hashmap *m)
{
    m->slots = nullptr;
    m->cap = 0;
    m->len = 0;
}

void embi_hashmap_free(embi_hashmap *m)
{
    embi_free(m->slots, (size_t)m->cap * sizeof(embi_hash_slot));
    embi_hashmap_init(m);
}

static embi_hash_slot *find_slot(const embi_hashmap *m, uint64_t key)
{
    if (m->cap == 0) {
        return nullptr;
    }
    uint32_t mask = m->cap - 1;
    uint32_t pos = (uint32_t)(embi_hash64(key) & mask);
    /* Robin Hood invariant: a slot with a smaller distance than ours means the key is absent. The
     * load factor keeps at least one empty slot, so this loop terminates. */
    for (uint32_t d = 1;; d++, pos = (pos + 1) & mask) {
        embi_hash_slot *s = &m->slots[pos];
        if (s->dist < d) {
            return nullptr;
        }
        if (s->dist == d && s->key == key) {
            return s;
        }
    }
}

bool embi_hashmap_get(const embi_hashmap *m, uint64_t key, uint64_t *out)
{
    embi_hash_slot *s = find_slot(m, key);
    if (!s) {
        return false;
    }
    if (out) {
        *out = s->val;
    }
    return true;
}

/* Inserts a key known to be absent, into a table with room. Does not change len. */
static void place(embi_hash_slot *slots, uint32_t mask, uint64_t key, uint64_t val)
{
    embi_hash_slot cur = {.key = key, .val = val, .dist = 1};
    uint32_t pos = (uint32_t)(embi_hash64(key) & mask);
    for (;;) {
        embi_hash_slot *s = &slots[pos];
        if (s->dist == 0) {
            *s = cur;
            return;
        }
        if (s->dist < cur.dist) {
            embi_hash_slot tmp = *s;
            *s = cur;
            cur = tmp;
        }
        pos = (pos + 1) & mask;
        cur.dist++;
    }
}

static void grow(embi_hashmap *m)
{
    uint32_t new_cap = m->cap ? m->cap * 2u : EMBI_MAP_MIN_CAP;
    embi_hash_slot *fresh = embi_calloc(new_cap, sizeof(embi_hash_slot));
    embi_hash_slot *old = m->slots;
    uint32_t old_cap = m->cap;

    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].dist) {
            place(fresh, new_cap - 1, old[i].key, old[i].val);
        }
    }
    embi_free(old, (size_t)old_cap * sizeof(embi_hash_slot));
    m->slots = fresh;
    m->cap = new_cap;
}

bool embi_hashmap_put(embi_hashmap *m, uint64_t key, uint64_t val)
{
    embi_hash_slot *existing = find_slot(m, key);
    if (existing) {
        existing->val = val;
        return false;
    }
    if ((uint64_t)(m->len + 1) * 100 > (uint64_t)m->cap * EMBI_MAP_LOAD_PCT) {
        grow(m);
    }
    place(m->slots, m->cap - 1, key, val);
    m->len++;
    return true;
}

/* Removes the entry in slot s (a slot of m with a key). */
static void remove_slot(embi_hashmap *m, embi_hash_slot *s)
{
    uint32_t mask = m->cap - 1;
    uint32_t pos = (uint32_t)(s - m->slots);
    uint32_t next = (pos + 1) & mask;
    /* Backward-shift: pull following entries with distance > 1 one slot back. */
    while (m->slots[next].dist > 1) {
        m->slots[pos] = m->slots[next];
        m->slots[pos].dist--;
        pos = next;
        next = (next + 1) & mask;
    }
    m->slots[pos].dist = 0;
    m->slots[pos].key = 0;
    m->slots[pos].val = 0;
    m->len--;
}

bool embi_hashmap_remove(embi_hashmap *m, uint64_t key)
{
    embi_hash_slot *s = find_slot(m, key);
    if (!s) {
        return false;
    }
    remove_slot(m, s);
    return true;
}

bool embi_hashmap_take(embi_hashmap *m, uint64_t key, uint64_t *out)
{
    embi_hash_slot *s = find_slot(m, key);
    if (!s) {
        return false;
    }
    *out = s->val;
    remove_slot(m, s);
    return true;
}

bool embi_hashmap_remove_if(embi_hashmap *m, uint64_t key, uint64_t val)
{
    embi_hash_slot *s = find_slot(m, key);
    if (!s || s->val != val) {
        return false;
    }
    remove_slot(m, s);
    return true;
}

void embi_hashmap_insert_new(embi_hashmap *m, uint64_t key, uint64_t val)
{
    if ((uint64_t)(m->len + 1) * 100 > (uint64_t)m->cap * EMBI_MAP_LOAD_PCT) {
        grow(m);
    }
    place(m->slots, m->cap - 1, key, val);
    m->len++;
}

uint64_t *embi_hashmap_ref(embi_hashmap *m, uint64_t key, uint64_t init, bool *added)
{
    embi_hash_slot *s = find_slot(m, key);
    if (s) {
        if (added) {
            *added = false;
        }
        return &s->val;
    }
    embi_hashmap_insert_new(m, key, init);
    if (added) {
        *added = true;
    }
    return &find_slot(m, key)->val;
}

void embi_hashmap_clear(embi_hashmap *m)
{
    if (m->cap) {
        SDL_memset(m->slots, 0, (size_t)m->cap * sizeof(embi_hash_slot));
    }
    m->len = 0;
}

uint32_t embi_hashmap_len(const embi_hashmap *m)
{
    return m->len;
}

embi_hashmap_iter embi_hashmap_iter_begin(const embi_hashmap *m)
{
    embi_hashmap_iter it = {m, 0};
    return it;
}

bool embi_hashmap_iter_next(embi_hashmap_iter *it, uint64_t *key, uint64_t *val)
{
    while (it->map && it->pos < it->map->cap) {
        const embi_hash_slot *s = &it->map->slots[it->pos++];
        if (s->dist) {
            *key = s->key;
            *val = s->val;
            return true;
        }
    }
    return false;
}
