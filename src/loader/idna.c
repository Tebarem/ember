/*
 * IDNA for URL hosts: the UTS #46 ToASCII with the URL standard's options (non-transitional,
 * UseSTD3ASCIIRules off, CheckHyphens off, CheckBidi and CheckJoiners on, no DNS length check),
 * and RFC 3492 Punycode.
 *
 * Steps per the URL standard's domain to ASCII: the UTS #46 mapping (src/loader/idna_table.inc,
 * from IdnaMappingTable.txt), then NFC, then per label: a label that starts with "xn--" must be
 * ASCII and is kept as it is (ToASCII does not decode A-labels, so an invalid Punycode payload is
 * not an error here, as in the WHATWG test data; UTS #46 section 4.1 step 4 would decode the label
 * and validate the result, which is a known gap, see known_idna_gap in
 * tests/unit/loader/test_url_idna.c); any other label must not start with a mark,
 * must pass the ContextJ rules for ZWNJ and ZWJ (RFC 5892 A.1, A.2) and the bidi rule (RFC 5893)
 * when it contains a right-to-left character, and is then Punycode-encoded when it is not ASCII.
 *
 * The property data (bidi classes, joining types, marks, combining classes, canonical
 * decompositions and compositions) comes from src/loader/idna_props.inc, generated with the
 * mapping table. The joining types come from a source that is one Unicode version behind (see
 * tools/unicodegen/data/JoiningType.txt).
 */
#include "loader/loader.h"

#include <string.h>

#include "idna_table.inc"
#include "idna_props.inc"

/* Kinds of an entry in the mapping stream; keep in step with tools/unicodegen/idnagen.c. */
enum {
    MAP_VALID = 0,
    MAP_IGNORED = 1,
    MAP_DELTA = 2,
    MAP_SEQ = 3,
    MAP_SEQREF = 4,
};

enum {
    IDNA_DISALLOWED = 0,
    IDNA_VALID,
    IDNA_IGNORED,
    IDNA_MAPPED,
};

/* Bidi class codes; keep in step with tools/unicodegen/idnagen.c. 0 is every other class. */
enum {
    BIDI_OTHER = 0,
    BIDI_L,
    BIDI_R,
    BIDI_AL,
    BIDI_EN,
    BIDI_ES,
    BIDI_ET,
    BIDI_AN,
    BIDI_CS,
    BIDI_NSM,
    BIDI_BN,
    BIDI_ON,
};

/* Joining type codes; keep in step with tools/unicodegen/idnagen.c. 0 is U. */
enum { JT_U = 0, JT_T, JT_R, JT_L, JT_D, JT_C };

#define IDNA_MAX_MAP 32u

#define CCC_VIRAMA 9u
#define CP_ZWNJ 0x200Cu
#define CP_ZWJ 0x200Du

/* Hangul syllables are composed and decomposed algorithmically (Unicode 3.12). */
#define HANGUL_SBASE 0xAC00u
#define HANGUL_LBASE 0x1100u
#define HANGUL_VBASE 0x1161u
#define HANGUL_TBASE 0x11A7u
#define HANGUL_LCOUNT 19u
#define HANGUL_VCOUNT 21u
#define HANGUL_TCOUNT 28u
#define HANGUL_NCOUNT (HANGUL_VCOUNT * HANGUL_TCOUNT)
#define HANGUL_SCOUNT (HANGUL_LCOUNT * HANGUL_NCOUNT)

/* LEB128, low group first (see tools/unicodegen/idnagen.c). */
static uint32_t idna_varint(const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint32_t v = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) {
            break;
        }
        shift += 7;
    }
    *pp = p;
    return v;
}

/* The index of the last sample whose code point is <= cp. Sample 0 is always code point 0. */
static size_t idna_sample(const uint32_t *samp_cp, size_t n, uint32_t cp)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (samp_cp[mid] <= cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo - 1;
}

/* Classifies one code point. For IDNA_MAPPED, the mapping goes to map[0..*nmap). The entries of
 * the mapping stream are decoded from the nearest sample on. */
static int idna_lookup(uint32_t cp, uint32_t *map, uint32_t *nmap)
{
    size_t si = idna_sample(embi_idna_map_samp_cp, sizeof embi_idna_map_samp_cp / sizeof embi_idna_map_samp_cp[0], cp);
    const uint8_t *p = embi_idna_map_stream + embi_idna_map_samp_off[si];
    const uint8_t *end = embi_idna_map_stream + sizeof embi_idna_map_stream;
    uint32_t cur = embi_idna_map_samp_cp[si];
    while (p < end) {
        uint32_t start = cur + idna_varint(&p);
        uint32_t t = idna_varint(&p);
        uint32_t kind = t & 7u;
        uint32_t last = start + (t >> 3);
        const uint8_t *seq = nullptr;
        uint32_t delta = 0;
        switch (kind) {
        case MAP_DELTA:
            delta = idna_varint(&p);
            break;
        case MAP_SEQ:
            seq = p;
            for (uint32_t n = *p++; n > 0; n--) {
                (void)idna_varint(&p);
            }
            break;
        case MAP_SEQREF:
            seq = embi_idna_map_stream + idna_varint(&p);
            break;
        default:
            break;
        }
        if (cp < start) {
            return IDNA_DISALLOWED;
        }
        if (cp <= last) {
            switch (kind) {
            case MAP_VALID:
                return IDNA_VALID;
            case MAP_IGNORED:
                return IDNA_IGNORED;
            case MAP_DELTA:
                /* The delta is zigzag-coded. */
                map[0] = cp + ((delta >> 1) ^ (0u - (delta & 1u)));
                *nmap = 1;
                return IDNA_MAPPED;
            default: {
                uint32_t n = *seq++;
                uint32_t k = 0;
                for (; k < n && k < IDNA_MAX_MAP; k++) {
                    map[k] = idna_varint(&seq);
                }
                *nmap = k;
                return IDNA_MAPPED;
            }
            }
        }
        cur = last + 1;
    }
    return IDNA_DISALLOWED;
}

/* ------------------------------------------------------------------ property lookups */

/* The property tuple id of cp. The run stream is (varint length, tuple id) pairs from code point 0
 * and everything after its end is tuple 0. Only code points that can occur in a mapped, normalized
 * label have meaningful values (see tools/unicodegen/idnagen.c). */
static unsigned idna_tuple(uint32_t cp)
{
    size_t si = idna_sample(embi_idna_prop_samp_cp, sizeof embi_idna_prop_samp_cp / sizeof embi_idna_prop_samp_cp[0], cp);
    const uint8_t *p = embi_idna_prop_stream + embi_idna_prop_samp_off[si];
    const uint8_t *end = embi_idna_prop_stream + sizeof embi_idna_prop_stream;
    uint32_t cur = embi_idna_prop_samp_cp[si];
    while (p < end) {
        cur += idna_varint(&p);
        if (cp < cur) {
            return *p;
        }
        p++;
    }
    return 0;
}

static uint8_t idna_ccc(uint32_t cp)
{
    /* Nothing below U+0300 combines. */
    return cp < 0x300 ? 0 : embi_idna_prop_ccc[idna_tuple(cp)];
}

static uint8_t idna_bidi(uint32_t cp)
{
    return embi_idna_prop_bj[idna_tuple(cp)] & 0x0Fu;
}

static uint8_t idna_joining(uint32_t cp)
{
    return (embi_idna_prop_bj[idna_tuple(cp)] >> 4) & 0x07u;
}

static bool idna_is_mark(uint32_t cp)
{
    return (embi_idna_prop_bj[idna_tuple(cp)] & 0x80u) != 0;
}

/* The canonical compositions behind NFC, as parallel arrays sorted by cp: the composite, its two
 * parts and their plane bits (bit 0 cp, bit 1 a, bit 2 b; all are in plane 0 or 1). Singletons and
 * composition exclusions are not kept: neither can occur in the mapped text, which is NFC-stable.
 * embi_idna_dc_by_pair lists the indexes sorted by (a, b). */
#define DC_COUNT (sizeof embi_idna_dc_cp / sizeof embi_idna_dc_cp[0])

static uint32_t dc_cp(size_t i)
{
    return embi_idna_dc_cp[i] | ((uint32_t)(embi_idna_dc_hi[i] & 1u) << 16);
}

static uint32_t dc_a(size_t i)
{
    return embi_idna_dc_a[i] | ((uint32_t)((embi_idna_dc_hi[i] >> 1) & 1u) << 16);
}

static uint32_t dc_b(size_t i)
{
    return embi_idna_dc_b[i] | ((uint32_t)((embi_idna_dc_hi[i] >> 2) & 1u) << 16);
}

/* The index of the composition whose composite is cp, or DC_COUNT. */
static size_t idna_decomp_of(uint32_t cp)
{
    size_t lo = 0;
    size_t hi = DC_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (dc_cp(mid) < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < DC_COUNT && dc_cp(lo) == cp ? lo : DC_COUNT;
}

/* Composes a starter (or a Hangul L or LV) with the next code point. */
static bool idna_compose_pair(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a >= HANGUL_LBASE && a < HANGUL_LBASE + HANGUL_LCOUNT && b >= HANGUL_VBASE &&
        b < HANGUL_VBASE + HANGUL_VCOUNT) {
        *out = HANGUL_SBASE + ((a - HANGUL_LBASE) * HANGUL_VCOUNT + (b - HANGUL_VBASE)) * HANGUL_TCOUNT;
        return true;
    }
    if (a >= HANGUL_SBASE && a < HANGUL_SBASE + HANGUL_SCOUNT && (a - HANGUL_SBASE) % HANGUL_TCOUNT == 0 &&
        b > HANGUL_TBASE && b < HANGUL_TBASE + HANGUL_TCOUNT) {
        *out = a + (b - HANGUL_TBASE);
        return true;
    }
    size_t lo = 0;
    size_t hi = DC_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t i = embi_idna_dc_by_pair[mid];
        uint32_t ma = dc_a(i);
        if (ma < a || (ma == a && dc_b(i) < b)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < DC_COUNT) {
        size_t i = embi_idna_dc_by_pair[lo];
        if (dc_a(i) == a && dc_b(i) == b) {
            *out = dc_cp(i);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ NFC (UAX #15) */

typedef EMBI_VEC(uint32_t) idna_cps;

static void nfc_decompose(uint32_t cp, idna_cps *out)
{
    if (cp >= HANGUL_SBASE && cp < HANGUL_SBASE + HANGUL_SCOUNT) {
        uint32_t s = cp - HANGUL_SBASE;
        embi_vec_push(out, HANGUL_LBASE + s / HANGUL_NCOUNT);
        embi_vec_push(out, HANGUL_VBASE + (s % HANGUL_NCOUNT) / HANGUL_TCOUNT);
        if (s % HANGUL_TCOUNT != 0) {
            embi_vec_push(out, HANGUL_TBASE + s % HANGUL_TCOUNT);
        }
        return;
    }
    size_t d = idna_decomp_of(cp);
    if (d == DC_COUNT) {
        embi_vec_push(out, cp);
        return;
    }
    nfc_decompose(dc_a(d), out);
    nfc_decompose(dc_b(d), out);
}

/* Canonical ordering: each run of non-starters is stably sorted by combining class. Short runs use
 * an insertion sort; a long run (a hostile host name) uses a counting sort so the cost stays
 * linear. */
static void nfc_reorder(idna_cps *dec)
{
    uint32_t i = 0;
    while (i < dec->len) {
        if (idna_ccc(dec->data[i]) == 0) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < dec->len && idna_ccc(dec->data[j]) != 0) {
            j++;
        }
        uint32_t n = j - i;
        uint32_t *run = dec->data + i;
        if (n <= 24) {
            for (uint32_t a = 1; a < n; a++) {
                uint8_t cc = idna_ccc(run[a]);
                for (uint32_t b = a; b > 0 && idna_ccc(run[b - 1]) > cc; b--) {
                    uint32_t t = run[b - 1];
                    run[b - 1] = run[b];
                    run[b] = t;
                }
            }
        } else {
            uint32_t count[256] = {0};
            uint32_t *tmp = embi_alloc((size_t)n * sizeof *tmp);
            uint8_t *cls = embi_alloc(n);
            for (uint32_t a = 0; a < n; a++) {
                cls[a] = idna_ccc(run[a]);
                count[cls[a]]++;
            }
            uint32_t sum = 0;
            for (unsigned c = 0; c < 256; c++) {
                uint32_t k = count[c];
                count[c] = sum;
                sum += k;
            }
            for (uint32_t a = 0; a < n; a++) {
                tmp[count[cls[a]]++] = run[a];
            }
            memcpy(run, tmp, (size_t)n * sizeof *tmp);
            embi_free(tmp, (size_t)n * sizeof *tmp);
            embi_free(cls, n);
        }
        i = j;
    }
}

/* Decomposes, puts runs of non-starters in canonical order, then composes, in place. */
static void nfc_normalize(idna_cps *v)
{
    idna_cps dec = {nullptr, 0, 0};
    for (uint32_t i = 0; i < v->len; i++) {
        nfc_decompose(v->data[i], &dec);
    }
    nfc_reorder(&dec);
    v->len = 0;
    /* Composition: a character joins the last starter when nothing between them blocks it. */
    bool have_starter = false;
    uint32_t starter = 0;
    uint8_t last_cc = 0;
    for (uint32_t i = 0; i < dec.len; i++) {
        uint32_t ch = dec.data[i];
        uint8_t cc = idna_ccc(ch);
        if (have_starter) {
            bool adjacent = v->len == starter + 1;
            uint32_t comp;
            if ((adjacent || last_cc < cc) && idna_compose_pair(v->data[starter], ch, &comp)) {
                v->data[starter] = comp;
                continue;
            }
        }
        if (cc == 0) {
            have_starter = true;
            starter = v->len;
            last_cc = 0;
        } else {
            last_cc = cc;
        }
        embi_vec_push(v, ch);
    }
    embi_vec_free(&dec);
}

/* ------------------------------------------------------------------ Punycode (RFC 3492) */

enum {
    PC_BASE = 36,
    PC_TMIN = 1,
    PC_TMAX = 26,
    PC_SKEW = 38,
    PC_DAMP = 700,
    PC_INITIAL_BIAS = 72,
    PC_INITIAL_N = 128,
};

static uint32_t pc_adapt(uint32_t delta, uint32_t numpoints, bool first)
{
    delta = first ? delta / PC_DAMP : delta / 2;
    delta += delta / numpoints;
    uint32_t k = 0;
    while (delta > ((PC_BASE - PC_TMIN) * PC_TMAX) / 2) {
        delta /= PC_BASE - PC_TMIN;
        k += PC_BASE;
    }
    return k + (PC_BASE - PC_TMIN + 1) * delta / (delta + PC_SKEW);
}

static char pc_digit(uint32_t d)
{
    return (char)(d < 26 ? 'a' + d : '0' + (d - 26));
}

/* The encoder scans the label once per distinct code point, so its cost is O(n * distinct). A label
 * longer than this many code points is rejected (false) instead of being encoded slowly; a DNS
 * label is at most 63 bytes. */
#define PC_MAX_ENCODE_CPS 4096u

bool embi_punycode_encode(const uint32_t *cps, uint32_t n, embi_sb *out)
{
    if (n > PC_MAX_ENCODE_CPS) {
        return false;
    }
    uint32_t b = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (cps[i] < 0x80) {
            embi_sb_append_char(out, (char)cps[i]);
            b++;
        }
    }
    if (b > 0) {
        embi_sb_append_char(out, '-');
    }
    uint32_t h = b;
    uint32_t nn = PC_INITIAL_N;
    uint64_t delta = 0;
    uint32_t bias = PC_INITIAL_BIAS;
    while (h < n) {
        uint32_t m = UINT32_MAX;
        for (uint32_t i = 0; i < n; i++) {
            if (cps[i] >= nn && cps[i] < m) {
                m = cps[i];
            }
        }
        delta += (uint64_t)(m - nn) * (h + 1);
        if (delta > UINT32_MAX) {
            return false;
        }
        nn = m;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = cps[i];
            if (c < nn) {
                delta++;
            }
            if (c == nn) {
                uint64_t q = delta;
                for (uint32_t k = PC_BASE;; k += PC_BASE) {
                    uint32_t t = k <= bias ? PC_TMIN : (k >= bias + PC_TMAX ? PC_TMAX : k - bias);
                    if (q < t) {
                        break;
                    }
                    embi_sb_append_char(out, pc_digit((uint32_t)(t + (q - t) % (PC_BASE - t))));
                    q = (q - t) / (PC_BASE - t);
                }
                embi_sb_append_char(out, pc_digit((uint32_t)q));
                bias = pc_adapt((uint32_t)delta, h + 1, h == b);
                delta = 0;
                h++;
            }
        }
        delta++;
        nn++;
    }
    return true;
}

/* Decodes Punycode (without the "xn--" prefix). The input is capped at 255 bytes: insertion into
 * the output is quadratic in its length, a DNS label is at most 63 bytes, and longer input is
 * rejected (false) instead of being decoded slowly. */
#define PC_MAX_DECODE_INPUT 255u

bool embi_punycode_decode(embi_str in, embi_sb *out)
{
    if (in.len > PC_MAX_DECODE_INPUT) {
        return false;
    }
    /* Basic code points are everything before the last '-'. */
    uint32_t basic_end = 0;
    bool has_delim = false;
    for (uint32_t i = 0; i < in.len; i++) {
        if (in.ptr[i] == '-') {
            basic_end = i;
            has_delim = true;
        }
    }
    EMBI_VEC(uint32_t) cps = {nullptr, 0, 0};
    for (uint32_t i = 0; has_delim && i < basic_end; i++) {
        unsigned char c = (unsigned char)in.ptr[i];
        if (c >= 0x80) {
            embi_vec_free(&cps);
            return false;
        }
        embi_vec_push(&cps, (uint32_t)c);
    }
    uint32_t pos = has_delim ? basic_end + 1 : 0;
    uint32_t nn = PC_INITIAL_N;
    uint32_t i = 0;
    uint32_t bias = PC_INITIAL_BIAS;
    while (pos < in.len) {
        uint32_t oldi = i;
        uint64_t w = 1;
        for (uint32_t k = PC_BASE;; k += PC_BASE) {
            if (pos >= in.len) {
                embi_vec_free(&cps);
                return false;
            }
            unsigned char c = (unsigned char)in.ptr[pos++];
            uint32_t digit;
            if (c >= 'a' && c <= 'z') {
                digit = c - 'a';
            } else if (c >= 'A' && c <= 'Z') {
                digit = c - 'A';
            } else if (c >= '0' && c <= '9') {
                digit = c - '0' + 26;
            } else {
                embi_vec_free(&cps);
                return false;
            }
            uint64_t add = (uint64_t)digit * w;
            if (add > UINT32_MAX || (uint64_t)i + add > UINT32_MAX) {
                embi_vec_free(&cps);
                return false;
            }
            i += (uint32_t)add;
            uint32_t t = k <= bias ? PC_TMIN : (k >= bias + PC_TMAX ? PC_TMAX : k - bias);
            if (digit < t) {
                break;
            }
            w *= PC_BASE - t;
            if (w > UINT32_MAX) {
                embi_vec_free(&cps);
                return false;
            }
        }
        uint32_t len = cps.len + 1;
        bias = pc_adapt(i - oldi, len, oldi == 0);
        if (i / len > 0x10FFFFu - nn) {
            embi_vec_free(&cps);
            return false;
        }
        nn += i / len;
        i %= len;
        if (nn > 0x10FFFFu || (nn >= 0xD800u && nn <= 0xDFFFu)) {
            embi_vec_free(&cps);
            return false;
        }
        /* Insert nn at position i. */
        embi_vec_reserve(&cps, cps.len + 1);
        for (uint32_t j = cps.len; j > i; j--) {
            cps.data[j] = cps.data[j - 1];
        }
        cps.data[i] = nn;
        cps.len++;
        i++;
    }
    for (uint32_t j = 0; j < cps.len; j++) {
        embi_sb_append_cp(out, cps.data[j]);
    }
    embi_vec_free(&cps);
    return true;
}

/* ------------------------------------------------------------------ UTS #46 ToASCII */

/* Maps the domain to code points (UTS #46 section 4, the mapping step). */
static bool idna_map(embi_str domain, idna_cps *mapped)
{
    const char *p = domain.ptr;
    const char *e = embi_str_end(domain);
    while (p < e) {
        uint32_t cp = embi_utf8_decode(&p, e);
        uint32_t map[IDNA_MAX_MAP];
        uint32_t nmap = 0;
        switch (idna_lookup(cp, map, &nmap)) {
        case IDNA_VALID:
            embi_vec_push(mapped, cp);
            break;
        case IDNA_IGNORED:
            break;
        case IDNA_MAPPED:
            for (uint32_t i = 0; i < nmap; i++) {
                embi_vec_push(mapped, map[i]);
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

/* RFC 5893 bidi rule for one label that contains a right-to-left character. */
static bool idna_bidi_rule(const uint32_t *cps, uint32_t n)
{
    if (n == 0) {
        return true;
    }
    uint8_t first = idna_bidi(cps[0]);
    if (first == BIDI_L) {
        /* Rule 5: an LTR label. Rule 6: it ends with L or EN, then NSMs. */
        uint32_t end = n;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t c = idna_bidi(cps[i]);
            if (c != BIDI_L && c != BIDI_EN && c != BIDI_ES && c != BIDI_CS && c != BIDI_ET &&
                c != BIDI_ON && c != BIDI_BN && c != BIDI_NSM) {
                return false;
            }
        }
        while (end > 0 && idna_bidi(cps[end - 1]) == BIDI_NSM) {
            end--;
        }
        if (end == 0) {
            return false;
        }
        uint8_t last = idna_bidi(cps[end - 1]);
        return last == BIDI_L || last == BIDI_EN;
    }
    if (first != BIDI_R && first != BIDI_AL) {
        return false;
    }
    /* Rules 2 to 4: an RTL label. */
    bool has_en = false;
    bool has_an = false;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = idna_bidi(cps[i]);
        switch (c) {
        case BIDI_R:
        case BIDI_AL:
        case BIDI_ES:
        case BIDI_CS:
        case BIDI_ET:
        case BIDI_ON:
        case BIDI_BN:
        case BIDI_NSM:
            break;
        case BIDI_AN:
            has_an = true;
            break;
        case BIDI_EN:
            has_en = true;
            break;
        default:
            return false;
        }
    }
    if (has_en && has_an) {
        return false;
    }
    uint32_t end = n;
    while (end > 0 && idna_bidi(cps[end - 1]) == BIDI_NSM) {
        end--;
    }
    if (end == 0) {
        return false;
    }
    uint8_t last = idna_bidi(cps[end - 1]);
    return last == BIDI_R || last == BIDI_AL || last == BIDI_EN || last == BIDI_AN;
}

/* The ContextJ rules (RFC 5892 A.1 and A.2) for every ZWNJ and ZWJ in the label. */
static bool idna_joiners_ok(const uint32_t *cps, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (cps[i] != CP_ZWNJ && cps[i] != CP_ZWJ) {
            continue;
        }
        if (i > 0 && idna_ccc(cps[i - 1]) == CCC_VIRAMA) {
            continue;
        }
        if (cps[i] == CP_ZWJ) {
            return false;
        }
        /* ZWNJ: (L or D) T* ZWNJ T* (R or D). */
        bool left = false;
        for (uint32_t j = i; j > 0; j--) {
            uint8_t jt = idna_joining(cps[j - 1]);
            if (jt == JT_T) {
                continue;
            }
            left = jt == JT_L || jt == JT_D;
            break;
        }
        if (!left) {
            return false;
        }
        bool right = false;
        for (uint32_t k = i + 1; k < n; k++) {
            uint8_t jt = idna_joining(cps[k]);
            if (jt == JT_T) {
                continue;
            }
            right = jt == JT_R || jt == JT_D;
            break;
        }
        if (!right) {
            return false;
        }
    }
    return true;
}

static bool idna_label_has_rtl(const uint32_t *cps, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = idna_bidi(cps[i]);
        if (c == BIDI_R || c == BIDI_AL || c == BIDI_AN) {
            return true;
        }
    }
    return false;
}

/* Whether the label holds an R, AL or AN character. An A-label (xn--) is not decoded, so it never
 * makes a domain Bidi: the WPT data (toascii.json, IdnaTestV2.json) expects A-labels that decode to
 * RTL text to pass next to LTR labels. */
static bool idna_label_is_bidi(const uint32_t *cps, uint32_t n)
{
    bool xn = n >= 4 && cps[0] == 'x' && cps[1] == 'n' && cps[2] == '-' && cps[3] == '-';
    return !xn && idna_label_has_rtl(cps, n);
}

/* Checks one mapped, normalized label and appends its ToASCII form to out. */
static bool idna_label(const uint32_t *cps, uint32_t n, bool bidi_domain, embi_sb *out)
{
    static const uint32_t xn_prefix[4] = {'x', 'n', '-', '-'};
    bool xn = n >= 4 && cps[0] == xn_prefix[0] && cps[1] == xn_prefix[1] && cps[2] == xn_prefix[2] &&
              cps[3] == xn_prefix[3];
    bool ascii = true;
    for (uint32_t i = 0; i < n; i++) {
        if (cps[i] >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (xn) {
        if (!ascii) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            embi_sb_append_char(out, (char)cps[i]);
        }
        return true;
    }
    if (n > 0 && idna_is_mark(cps[0])) {
        return false;
    }
    if (!idna_joiners_ok(cps, n)) {
        return false;
    }
    if (bidi_domain && !idna_bidi_rule(cps, n)) {
        return false;
    }
    if (ascii) {
        for (uint32_t i = 0; i < n; i++) {
            embi_sb_append_char(out, (char)cps[i]);
        }
        return true;
    }
    embi_sb_append(out, EMBI_STR("xn--"));
    return embi_punycode_encode(cps, n, out);
}

bool embi_idna_to_ascii(embi_str domain, embi_sb *out)
{
    idna_cps mapped = {nullptr, 0, 0};
    if (!idna_map(domain, &mapped)) {
        embi_vec_free(&mapped);
        return false;
    }
    nfc_normalize(&mapped);
    /* Labels are split on '.', which the mapping has already produced for the ideographic full
     * stops. */
    bool ok = true;
    /* A Bidi domain name has an R, AL or AN character in some label. The bidi rule then applies
     * to every label, ASCII ones included (RFC 5893 section 2, UTS #46 4.1 step 4). */
    bool bidi_domain = false;
    {
        uint32_t ls = 0;
        for (uint32_t i = 0; i <= mapped.len && !bidi_domain; i++) {
            if (i < mapped.len && mapped.data[i] != '.') {
                continue;
            }
            bidi_domain = i > ls && idna_label_is_bidi(mapped.data + ls, i - ls);
            ls = i + 1;
        }
    }
    uint32_t start = 0;
    for (uint32_t i = 0; i <= mapped.len && ok; i++) {
        if (i < mapped.len && mapped.data[i] != '.') {
            continue;
        }
        /* Every label but the first (which starts at 0) is preceded by a '.'. */
        if (start > 0) {
            embi_sb_append_char(out, '.');
        }
        const uint32_t *label = mapped.len > 0 ? mapped.data + start : nullptr;
        ok = idna_label(label, i - start, bidi_domain, out);
        start = i + 1;
    }
    embi_vec_free(&mapped);
    return ok;
}
