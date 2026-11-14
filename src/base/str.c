#include "base/base.h"

#define EMBI_REPLACEMENT 0xFFFDu

uint32_t embi_utf8_decode(const char **pp, const char *end)
{
    const unsigned char *p = (const unsigned char *)*pp;
    const unsigned char *e = (const unsigned char *)end;
    EMBI_ASSERT(p < e);
    if (p >= e) {
        return EMBI_REPLACEMENT;
    }
    unsigned lead = p[0];
    if (lead < 0x80u) {
        *pp += 1;
        return lead;
    }

    unsigned need;
    uint32_t cp;
    unsigned lo = 0x80u;
    unsigned hi = 0xBFu;
    if (lead >= 0xC2u && lead <= 0xDFu) {
        need = 1;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0u && lead <= 0xEFu) {
        need = 2;
        cp = lead & 0x0Fu;
        if (lead == 0xE0u) {
            lo = 0xA0u; /* no overlongs */
        }
        if (lead == 0xEDu) {
            hi = 0x9Fu; /* no surrogates */
        }
    } else if (lead >= 0xF0u && lead <= 0xF4u) {
        need = 3;
        cp = lead & 0x07u;
        if (lead == 0xF0u) {
            lo = 0x90u; /* no overlongs */
        }
        if (lead == 0xF4u) {
            hi = 0x8Fu; /* at most U+10FFFF */
        }
    } else {
        *pp += 1;
        return EMBI_REPLACEMENT;
    }

    for (unsigned i = 1; i <= need; i++) {
        /* Truncated input: the i bytes consumed so far form the maximal invalid subpart. */
        if ((size_t)(e - p) <= i) {
            *pp += i;
            return EMBI_REPLACEMENT;
        }
        unsigned b = p[i];
        if (b < lo || b > hi) {
            *pp += i;
            return EMBI_REPLACEMENT;
        }
        lo = 0x80u;
        hi = 0xBFu;
        cp = (cp << 6) | (b & 0x3Fu);
    }
    *pp += need + 1u;
    return cp;
}

size_t embi_utf8_encode(uint32_t cp, char *out)
{
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = EMBI_REPLACEMENT;
    }
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static void put16(uint16_t *out, size_t cap, size_t idx, uint16_t unit)
{
    if (out && idx < cap) {
        out[idx] = unit;
    }
}

size_t embi_utf8_to_utf16(embi_str s, uint16_t *out, size_t cap)
{
    const char *p = s.ptr;
    const char *end = s.ptr ? s.ptr + s.len : nullptr;
    size_t n = 0;
    while (p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            put16(out, cap, n++, (uint16_t)(0xD800u + (cp >> 10)));
            put16(out, cap, n++, (uint16_t)(0xDC00u + (cp & 0x3FFu)));
        } else {
            put16(out, cap, n++, (uint16_t)cp);
        }
    }
    return n;
}

size_t embi_utf16_to_utf8(const uint16_t *in, size_t count, char *out, size_t cap)
{
    size_t w = 0;
    size_t i = 0;
    while (i < count) {
        uint32_t cp = in[i++];
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            uint32_t next = i < count ? in[i] : 0u;
            if (next >= 0xDC00u && next <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (next - 0xDC00u);
                i++;
            } else {
                cp = EMBI_REPLACEMENT;
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = EMBI_REPLACEMENT;
        }
        char tmp[4];
        size_t len = embi_utf8_encode(cp, tmp);
        if (out && w + len <= cap) {
            for (size_t k = 0; k < len; k++) {
                out[w + k] = tmp[k];
            }
        }
        w += len;
    }
    return w;
}

bool embi_str_eq(embi_str a, embi_str b)
{
    if (a.len != b.len) {
        return false;
    }
    return a.len == 0 || SDL_memcmp(a.ptr, b.ptr, a.len) == 0;
}

static unsigned fold_ascii(char c)
{
    unsigned u = (unsigned char)c;
    return (u >= 'A' && u <= 'Z') ? u + 32u : u;
}

int embi_str_icmp(embi_str a, embi_str b)
{
    uint32_t n = a.len < b.len ? a.len : b.len;
    for (uint32_t i = 0; i < n; i++) {
        unsigned x = fold_ascii(a.ptr[i]);
        unsigned y = fold_ascii(b.ptr[i]);
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.len == b.len) {
        return 0;
    }
    return a.len < b.len ? -1 : 1;
}

bool embi_str_ieq(embi_str a, embi_str b)
{
    return a.len == b.len && embi_str_icmp(a, b) == 0;
}

/* ------------------------------------------------------------------ string builder */

static void sb_reserve_extra(embi_sb *sb, size_t extra)
{
    if (extra > (size_t)(UINT32_MAX - sb->len)) {
        embi_oom(extra);
    }
    embi_vec_reserve(sb, sb->len + (uint32_t)extra);
}

static void sb_put(embi_sb *sb, const char *p, size_t n)
{
    if (n == 0) {
        return;
    }
    sb_reserve_extra(sb, n);
    SDL_memcpy(sb->data + sb->len, p, n);
    sb->len += (uint32_t)n;
}

void embi_sb_free(embi_sb *sb)
{
    embi_vec_free(sb);
}

void embi_sb_clear(embi_sb *sb)
{
    embi_vec_clear(sb);
}

void embi_sb_append(embi_sb *sb, embi_str s)
{
    sb_put(sb, s.ptr, s.len);
}

void embi_sb_append_char(embi_sb *sb, char c)
{
    sb_reserve_extra(sb, 1);
    sb->data[sb->len++] = c;
}

void embi_sb_append_cp(embi_sb *sb, uint32_t cp)
{
    char tmp[4];
    size_t n = embi_utf8_encode(cp, tmp);
    sb_put(sb, tmp, n);
}

void embi_sb_append_u64(embi_sb *sb, uint64_t v)
{
    char buf[20];
    size_t i = sizeof buf;
    do {
        buf[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    sb_put(sb, buf + i, sizeof buf - i);
}

void embi_sb_append_i64(embi_sb *sb, int64_t v)
{
    if (v < 0) {
        embi_sb_append_char(sb, '-');
        embi_sb_append_u64(sb, 0u - (uint64_t)v);
    } else {
        embi_sb_append_u64(sb, (uint64_t)v);
    }
}

void embi_sb_append_f64(embi_sb *sb, double v)
{
    char buf[40];
    int n = SDL_snprintf(buf, sizeof buf, "%.17g", v);
    if (n > 0) {
        size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
        sb_put(sb, buf, len);
    }
}

const char *embi_sb_cstr(embi_sb *sb)
{
    sb_reserve_extra(sb, 1);
    sb->data[sb->len] = '\0';
    return sb->data;
}
