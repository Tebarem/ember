/*
 * Unicode normalization (UAX #15) for String.prototype.normalize (ECMA-262 22.1.3.15).
 *
 * The four forms run in three steps over code points: decomposition (Hangul syllables are
 * algorithmic; other code points use the mapping table, recursively, and compatibility mappings
 * only for the K forms), canonical ordering (each run of non-zero combining classes is stably
 * sorted by class), and for NFC and NFKC canonical composition (each starter absorbs the following
 * marks it can combine with, unless a mark in between blocks it). The composition pairs in the
 * table already exclude Full_Composition_Exclusion, so no exclusion check is needed here.
 *
 * Table lookups are binary searches. The decomposition pool is laid out in code point order, so a
 * record ends where the next record of its table begins (see tools/unicodegen/normgen.c).
 */
#include "js/unicode_norm.h"

#include <stdbool.h>
#include <stdint.h>

#include "js/unicode_norm.inc"

#define HANGUL_S_BASE 0xAC00u
#define HANGUL_L_BASE 0x1100u
#define HANGUL_V_BASE 0x1161u
#define HANGUL_T_BASE 0x11A7u
#define HANGUL_L_COUNT 19u
#define HANGUL_V_COUNT 21u
#define HANGUL_T_COUNT 28u
#define HANGUL_N_COUNT (HANGUL_V_COUNT * HANGUL_T_COUNT)
#define HANGUL_S_COUNT (HANGUL_L_COUNT * HANGUL_N_COUNT)

/* Compatibility flag in the top bit of a decomposition offset (the pool is far smaller). */
#define DEC_COMPAT_BIT 0x8000u
#define DEC_OFF_MASK 0x7FFFu

/* Bounds the recursion of the decomposition. The longest real chain is three steps. */
#define DECOMP_DEPTH_MAX 16

typedef EMBI_VEC(uint32_t) cp_vec;

/* Index of key in the sorted array, or -1. */
static int32_t find16(const uint16_t *keys, uint32_t n, uint32_t key)
{
    uint32_t lo = 0;
    uint32_t hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (keys[mid] < key) {
            lo = mid + 1;
        } else if (keys[mid] > key) {
            hi = mid;
        } else {
            return (int32_t)mid;
        }
    }
    return -1;
}

static int32_t find32(const uint32_t *keys, uint32_t n, uint32_t key)
{
    uint32_t lo = 0;
    uint32_t hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (keys[mid] < key) {
            lo = mid + 1;
        } else if (keys[mid] > key) {
            hi = mid;
        } else {
            return (int32_t)mid;
        }
    }
    return -1;
}

/* Canonical combining class of cp (0 for every code point the table does not list). */
static uint8_t ccc_of(uint32_t cp)
{
    uint32_t lo = 0;
    uint32_t hi = EMBI_NORM_CCC_COUNT;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (embi_norm_ccc[mid].hi < cp) {
            lo = mid + 1;
        } else if (embi_norm_ccc[mid].lo > cp) {
            hi = mid;
        } else {
            return embi_norm_ccc[mid].ccc;
        }
    }
    return 0;
}

/* The mapping of cp: pool units [*start, *end) and whether it is a compatibility mapping. Returns
 * false when cp has no mapping. */
static bool dec_lookup(uint32_t cp, uint32_t *start, uint32_t *end, bool *compat)
{
    uint16_t off;
    uint16_t next;
    if (cp <= 0xFFFFu) {
        int32_t i = find16(embi_norm_dec_bmp_key, EMBI_NORM_DEC_BMP_COUNT, cp);
        if (i < 0) {
            return false;
        }
        off = embi_norm_dec_bmp_off[i];
        next = embi_norm_dec_bmp_off[i + 1];
    } else {
        int32_t i = find32(embi_norm_dec_smp_key, EMBI_NORM_DEC_SMP_COUNT, cp);
        if (i < 0) {
            return false;
        }
        off = embi_norm_dec_smp_off[i];
        next = embi_norm_dec_smp_off[i + 1];
    }
    *start = off & DEC_OFF_MASK;
    *end = next & DEC_OFF_MASK;
    *compat = (off & DEC_COMPAT_BIT) != 0;
    return true;
}

/* The code point that starts at pool unit i (a surrogate pair is one code point). */
static uint32_t pool_cp(uint32_t i, uint32_t *units)
{
    uint32_t u = embi_norm_pool[i];
    if (u >= 0xD800u && u < 0xDC00u) {
        *units = 2;
        return 0x10000u + ((u - 0xD800u) << 10) + (embi_norm_pool[i + 1] - 0xDC00u);
    }
    *units = 1;
    return u;
}

/* Appends the full decomposition of cp to out. compat is true for the K forms. */
static void decompose(uint32_t cp, bool compat, uint32_t depth, cp_vec *out)
{
    if (cp >= HANGUL_S_BASE && cp < HANGUL_S_BASE + HANGUL_S_COUNT) {
        uint32_t s = cp - HANGUL_S_BASE;
        uint32_t l = HANGUL_L_BASE + s / HANGUL_N_COUNT;
        uint32_t v = HANGUL_V_BASE + (s % HANGUL_N_COUNT) / HANGUL_T_COUNT;
        uint32_t t = HANGUL_T_BASE + s % HANGUL_T_COUNT;
        embi_vec_push(out, l);
        embi_vec_push(out, v);
        if (t != HANGUL_T_BASE) {
            embi_vec_push(out, t);
        }
        return;
    }
    uint32_t start;
    uint32_t end;
    bool is_compat;
    if (depth < DECOMP_DEPTH_MAX && dec_lookup(cp, &start, &end, &is_compat) &&
        (!is_compat || compat)) {
        uint32_t i = start;
        while (i < end) {
            uint32_t units;
            uint32_t sub = pool_cp(i, &units);
            decompose(sub, compat, depth + 1, out);
            i += units;
        }
        return;
    }
    embi_vec_push(out, cp);
}

/* Canonical ordering: each run of non-starters is stably sorted by combining class. */
static void reorder(cp_vec *v)
{
    uint32_t i = 0;
    while (i < v->len) {
        if (ccc_of(v->data[i]) == 0) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < v->len && ccc_of(v->data[j]) != 0) {
            j++;
        }
        for (uint32_t a = i + 1; a < j; a++) {
            uint32_t x = v->data[a];
            uint8_t cx = ccc_of(x);
            uint32_t b = a;
            while (b > i && ccc_of(v->data[b - 1]) > cx) {
                v->data[b] = v->data[b - 1];
                b--;
            }
            v->data[b] = x;
        }
        i = j;
    }
}

/* The primary composite of a and b (canonical), or false. */
static bool compose_pair(uint32_t a, uint32_t b, uint32_t *c)
{
    if (a >= HANGUL_L_BASE && a < HANGUL_L_BASE + HANGUL_L_COUNT && b >= HANGUL_V_BASE &&
        b < HANGUL_V_BASE + HANGUL_V_COUNT) {
        *c = HANGUL_S_BASE + ((a - HANGUL_L_BASE) * HANGUL_V_COUNT + (b - HANGUL_V_BASE)) *
                                 HANGUL_T_COUNT;
        return true;
    }
    if (a >= HANGUL_S_BASE && a < HANGUL_S_BASE + HANGUL_S_COUNT &&
        (a - HANGUL_S_BASE) % HANGUL_T_COUNT == 0 && b > HANGUL_T_BASE &&
        b < HANGUL_T_BASE + HANGUL_T_COUNT) {
        *c = a + (b - HANGUL_T_BASE);
        return true;
    }
    if (a <= 0xFFFFu && b <= 0xFFFFu) {
        uint32_t lo = 0;
        uint32_t hi = EMBI_NORM_COMP_BMP_COUNT;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const uint16_t *row = embi_norm_comp_bmp[mid];
            if (row[0] < a || (row[0] == a && row[1] < b)) {
                lo = mid + 1;
            } else if (row[0] > a || (row[0] == a && row[1] > b)) {
                hi = mid;
            } else {
                *c = row[2];
                return true;
            }
        }
        return false;
    }
    uint32_t lo = 0;
    uint32_t hi = EMBI_NORM_COMP_SMP_COUNT;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint32_t *row = embi_norm_comp_smp[mid];
        if (row[0] < a || (row[0] == a && row[1] < b)) {
            lo = mid + 1;
        } else if (row[0] > a || (row[0] == a && row[1] > b)) {
            hi = mid;
        } else {
            *c = row[2];
            return true;
        }
    }
    return false;
}

/* Canonical composition in place (the output never outgrows the input). */
static void compose(cp_vec *v)
{
    uint32_t w = 0;
    int64_t starter = -1; /* index in the output of the last starter, or -1 */
    uint8_t last_ccc = 0;
    for (uint32_t i = 0; i < v->len; i++) {
        uint32_t cp = v->data[i];
        uint8_t cc = ccc_of(cp);
        if (starter >= 0) {
            bool adjacent = (int64_t)w - 1 == starter;
            /* Not blocked: nothing between the starter and cp, or a mark of a lower class than
             * cp's own (marks are sorted, so the last one has the highest class). */
            if (adjacent || (last_ccc != 0 && last_ccc < cc)) {
                uint32_t c;
                if (compose_pair(v->data[starter], cp, &c)) {
                    v->data[starter] = c;
                    continue;
                }
            }
        }
        if (cc == 0) {
            starter = w;
        }
        v->data[w++] = cp;
        last_ccc = cc;
    }
    v->len = w;
}

bool embi_norm_run(const uint16_t *in, uint32_t n, enum embi_norm_form form, embi_norm_units *out)
{
    bool compat = form == EMBI_NORM_NFKC || form == EMBI_NORM_NFKD;
    bool compose_out = form == EMBI_NORM_NFC || form == EMBI_NORM_NFKC;
    cp_vec cps = {nullptr, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800u && cp < 0xDC00u && i + 1 < n && in[i + 1] >= 0xDC00u &&
            in[i + 1] < 0xE000u) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (in[i + 1] - 0xDC00u);
            i++;
        }
        decompose(cp, compat, 0, &cps);
    }
    reorder(&cps);
    if (compose_out) {
        compose(&cps);
    }
    out->len = 0;
    for (uint32_t i = 0; i < cps.len; i++) {
        uint32_t cp = cps.data[i];
        if (cp < 0x10000u) {
            embi_vec_push(out, (uint16_t)cp);
        } else {
            cp -= 0x10000u;
            embi_vec_push(out, (uint16_t)(0xD800u + (cp >> 10)));
            embi_vec_push(out, (uint16_t)(0xDC00u + (cp & 0x3FFu)));
        }
    }
    embi_vec_free(&cps);
    return true;
}
