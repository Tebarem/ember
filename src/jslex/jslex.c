/*
 * ES2025 lexer. See jslex.h for the contract.
 *
 * Positions: token start offsets are monotonic, so the line and column of each token come
 * from a cache that only moves forward. Lines end at LF, CR (CRLF counts once), LS and PS.
 * Columns count UTF-16 code units.
 */
#include "base/base.h"
#include "jslex/jslex.h"
#include "jslex/jslex_num.h"

#include "unicode_id.inc"

#define NOCP 0xFFFFFFFFu

/* ------------------------------------------------------------------ small helpers */

static bool fail(embi_jslex *lx, const char *msg, uint32_t at)
{
    lx->err = msg;
    lx->err_pos = at;
    return false;
}

/* ASCII identifier classes: 1 = may continue a name, 2 = may start one (plus 1). */
static const uint8_t ascii_id[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 3,
    0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static uint32_t hexval(uint32_t c)
{
    uint32_t d = c - '0';
    if (d < 10u) {
        return d;
    }
    d = (c | 0x20u) - 'a';
    return d < 6u ? d + 10u : 99u;
}

static bool is_dec(uint32_t c)
{
    return c - '0' < 10u;
}

static bool is_oct(uint32_t c)
{
    return c - '0' < 8u;
}

static bool is_lt(uint32_t cp)
{
    return cp == 0x0A || cp == 0x0D || cp == 0x2028 || cp == 0x2029;
}

static bool is_zs(uint32_t cp)
{
    return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

static uint32_t decode_at(const embi_jslex *lx, uint32_t p, uint32_t *w)
{
    uint8_t c = lx->src[p];
    if (c < 0x80) {
        *w = 1;
        return c;
    }
    if (c == 0xEDu && p + 2u < lx->len && lx->src[p + 1u] >= 0xA0u && lx->src[p + 1u] <= 0xBFu &&
        (lx->src[p + 2u] & 0xC0u) == 0x80u) {
        /* A lone surrogate in WTF-8 (source text built from a JS string by eval or Function). */
        *w = 3;
        return 0xD000u | ((uint32_t)(lx->src[p + 1u] & 0x3Fu) << 6) | (lx->src[p + 2u] & 0x3Fu);
    }
    const char *b = (const char *)lx->src + p;
    const char *q = b;
    uint32_t cp = embi_utf8_decode(&q, (const char *)lx->src + lx->len);
    *w = (uint32_t)(q - b);
    return cp;
}

static bool id_start_cp(uint32_t cp)
{
    return cp == '$' || cp == '_' || embi_jslex_is_id_start(cp);
}

static bool id_part_cp(uint32_t cp)
{
    return cp == '$' || cp == '_' || cp == 0x200C || cp == 0x200D || embi_jslex_is_id_continue(cp);
}

/* ------------------------------------------------------------------ scratch output */

static bool put(embi_jslex *lx, uint32_t *n, uint32_t unit)
{
    if (*n >= lx->scratch_cap) {
        return fail(lx, "token too long for the lexer scratch buffer", lx->pos);
    }
    lx->scratch[(*n)++] = (uint16_t)unit;
    return true;
}

static bool put_cp(embi_jslex *lx, uint32_t *n, uint32_t cp)
{
    if (cp >= 0x10000u) {
        cp -= 0x10000u;
        return put(lx, n, 0xD800u + (cp >> 10)) && put(lx, n, 0xDC00u + (cp & 0x3FFu));
    }
    return put(lx, n, cp);
}

/* ------------------------------------------------------------------ positions */

static void pos_linecol(embi_jslex *lx, uint32_t target, uint32_t *line, uint32_t *col)
{
    if (target < lx->pc_pos) {
        lx->pc_pos = 0;
        lx->pc_line = 1;
        lx->pc_col = 0;
    }
    uint32_t p = lx->pc_pos;
    uint32_t ln = lx->pc_line;
    uint32_t cl = lx->pc_col;
    while (p < target) {
        uint8_t c = lx->src[p];
        if (c >= 0x20u && c < 0x80u) { /* a run of printable ASCII: one column each */
            uint32_t q = p + 1;
            /* FP2a: eight bytes at a time while none has the high bit set or is below 0x20 (a minified bundle is one
             * long line of printable ASCII, and this loop used to visit each of its bytes). */
            while (q + 8u <= target) {
                uint64_t w;
                memcpy(&w, lx->src + q, sizeof w);
                if ((w & 0x8080808080808080ull) != 0 ||
                    ((w - 0x2020202020202020ull) & ~w & 0x8080808080808080ull) != 0) {
                    break;
                }
                q += 8u;
            }
            while (q < target && lx->src[q] >= 0x20u && lx->src[q] < 0x80u) {
                q++;
            }
            cl += q - p;
            p = q;
            continue;
        }
        if (c == '\n') {
            ln++;
            cl = 0;
            p++;
        } else if (c == '\r') {
            if (p + 1 < lx->len && lx->src[p + 1] == '\n') {
                p++; /* the LF that follows ends the line */
            } else {
                ln++;
                cl = 0;
                p++;
            }
        } else if (c < 0x80) {
            cl++;
            p++;
        } else {
            uint32_t w;
            uint32_t cp = decode_at(lx, p, &w);
            if (cp == 0x2028 || cp == 0x2029) {
                ln++;
                cl = 0;
            } else {
                cl += cp >= 0x10000u ? 2u : 1u;
            }
            p += w;
        }
    }
    lx->pc_pos = p;
    lx->pc_line = ln;
    lx->pc_col = cl;
    *line = ln;
    *col = cl + 1;
}

/* ------------------------------------------------------------------ trivia */

static uint32_t skip_line(const embi_jslex *lx, uint32_t p)
{
    while (p < lx->len) {
        uint32_t w;
        uint32_t cp = decode_at(lx, p, &w);
        if (is_lt(cp)) {
            break;
        }
        p += w;
    }
    return p;
}

/* Skips whitespace and comments from *pp. Sets *nl when a line terminator was crossed. */
static bool skip_trivia(embi_jslex *lx, uint32_t *pp, bool *nl)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    const bool html = (lx->flags & EMBI_JSLEX_MODULE) == 0;
    const bool at_input_start = *pp == 0;
    uint32_t p = *pp;

    for (;;) {
        if (p >= len) {
            break;
        }
        uint8_t c = s[p];
        if (c == ' ' || c == '\t' || c == 0x0B || c == 0x0C) {
            p++;
            continue;
        }
        if (c == '\n' || c == '\r') {
            *nl = true;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < len && s[p + 1] == '/') {
            p = skip_line(lx, p + 2);
            continue;
        }
        if (c == '/' && p + 1 < len && s[p + 1] == '*') {
            uint32_t q = p + 2;
            bool closed = false;
            while (q < len) {
                if (s[q] == '*' && q + 1 < len && s[q + 1] == '/') {
                    q += 2;
                    closed = true;
                    break;
                }
                uint32_t w;
                uint32_t cp = decode_at(lx, q, &w);
                if (is_lt(cp)) {
                    *nl = true;
                }
                q += w;
            }
            if (!closed) {
                return fail(lx, "unterminated comment", p);
            }
            p = q;
            continue;
        }
        if (p == 0 && c == '#' && p + 1 < len && s[p + 1] == '!') {
            p = skip_line(lx, p + 2);
            continue;
        }
        if (html && c == '<' && p + 3 < len && s[p + 1] == '!' && s[p + 2] == '-' &&
            s[p + 3] == '-') {
            p = skip_line(lx, p + 4);
            continue;
        }
        if (html && c == '-' && (*nl || at_input_start) && p + 2 < len && s[p + 1] == '-' &&
            s[p + 2] == '>') {
            p = skip_line(lx, p + 3);
            continue;
        }
        if (c >= 0x80) {
            uint32_t w;
            uint32_t cp = decode_at(lx, p, &w);
            if (is_lt(cp)) {
                *nl = true;
                p += w;
                continue;
            }
            if (cp == 0xFEFF || is_zs(cp)) {
                p += w;
                continue;
            }
        }
        break;
    }
    *pp = p;
    return true;
}

/* ------------------------------------------------------------------ names */

static bool valid_name_cp(uint32_t cp, bool first)
{
    return first ? id_start_cp(cp) : id_part_cp(cp);
}

/* p is the index of the 'u' of a \u escape. The escape body is \uXXXX or \u{X...}. */
static bool read_u(const embi_jslex *lx, uint32_t p, uint32_t *cp, uint32_t *end)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    uint32_t q = p + 1;
    if (q < len && s[q] == '{') {
        q++;
        uint32_t v = 0;
        uint32_t digits = 0;
        while (q < len && hexval(s[q]) < 16u) {
            v = v * 16 + hexval(s[q]);
            if (v > 0x10FFFFu) {
                return false;
            }
            digits++;
            q++;
        }
        if (digits == 0 || q >= len || s[q] != '}') {
            return false;
        }
        *cp = v;
        *end = q + 1;
        return true;
    }
    if (q + 4 > len) {
        return false;
    }
    uint32_t v = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t h = hexval(s[q + i]);
        if (h >= 16u) {
            return false;
        }
        v = v * 16 + h;
    }
    *cp = v;
    *end = q + 4;
    return true;
}

static bool keyword_utf16(const uint16_t *u, uint32_t n, embi_jskw *out)
{
    if (n < 2 || n > 10) {
        *out = EMBI_JSKW_NONE;
        return true;
    }
    uint8_t b[10];
    for (uint32_t i = 0; i < n; i++) {
        if (u[i] >= 0x80) {
            *out = EMBI_JSKW_NONE;
            return true;
        }
        b[i] = (uint8_t)u[i];
    }
    *out = embi_jslex_keyword(b, n);
    return true;
}

/* name_start is the first byte of the name (after '#' for private names). */
static bool scan_name(embi_jslex *lx, embi_jstok *t, uint32_t name_start, bool priv)
{
    const uint32_t len = lx->len;
    uint32_t p = name_start;
    /* Fast path: a plain ASCII name (no escape, no non-ASCII byte after it). */
    if (p < len && (ascii_id[lx->src[p]] & 2u)) {
        const uint8_t *s = lx->src;
        uint32_t q = p + 1;
        while (q < len && (ascii_id[s[q]] & 1u)) {
            q++;
        }
        if (q >= len || (s[q] < 0x80u && s[q] != '\\')) {
            t->kind = priv ? EMBI_JST_PRIVATE_NAME : EMBI_JST_NAME;
            t->end = q;
            t->kw = priv ? EMBI_JSKW_NONE : embi_jslex_keyword(s + p, q - p);
            return true;
        }
    }
    bool escaped = false;
    bool first = true;
    while (p < len) {
        uint32_t cp;
        if (lx->src[p] == '\\') {
            uint32_t next;
            if (p + 1 >= len || lx->src[p + 1] != 'u' || !read_u(lx, p + 1, &cp, &next)) {
                return fail(lx, "invalid Unicode escape in identifier", p);
            }
            if (!valid_name_cp(cp, first)) {
                return fail(lx, "invalid character in identifier escape", p);
            }
            escaped = true;
            p = next;
        } else {
            uint32_t w;
            cp = decode_at(lx, p, &w);
            if (!valid_name_cp(cp, first)) {
                break;
            }
            p += w;
        }
        first = false;
    }
    if (first) {
        return fail(lx, priv ? "invalid private name" : "invalid identifier", name_start);
    }

    uint32_t n = 0;
    if (escaped) {
        uint32_t q = name_start;
        while (q < p) {
            uint32_t cp;
            if (lx->src[q] == '\\') {
                uint32_t next;
                (void)read_u(lx, q + 1, &cp, &next);
                q = next;
            } else {
                uint32_t w;
                cp = decode_at(lx, q, &w);
                q += w;
            }
            if (!put_cp(lx, &n, cp)) {
                return false;
            }
        }
        t->flags |= EMBI_JST_F_ESCAPED;
        t->cooked = lx->scratch;
        t->cooked_len = n;
    }
    t->kind = priv ? EMBI_JST_PRIVATE_NAME : EMBI_JST_NAME;
    t->end = p;
    if (priv) {
        t->kw = EMBI_JSKW_NONE;
    } else if (escaped) {
        keyword_utf16(lx->scratch, n, &t->kw);
    } else {
        t->kw = embi_jslex_keyword(lx->src + name_start, p - name_start);
    }
    return true;
}

/* ------------------------------------------------------------------ numbers */

/* Checks that no identifier or digit follows a numeric literal ending at p. */
static bool numeric_end(embi_jslex *lx, uint32_t p)
{
    if (p >= lx->len) {
        return true;
    }
    uint32_t w;
    uint32_t cp = decode_at(lx, p, &w);
    if (id_start_cp(cp) || cp == '\\' || is_dec(cp)) {
        return fail(lx, "identifier or digit starts immediately after numeric literal", p);
    }
    return true;
}

/* Reads digits of the given radix from *pp, with optional single '_' separators between
 * digits. Feeds acc and/or the exponent accumulator ev when they are not nullptr. */
static bool scan_digits(embi_jslex *lx, uint32_t *pp, unsigned radix, bool allow_sep,
                        embi_jsnum *acc, uint32_t *count, uint64_t *ev)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    uint32_t p = *pp;
    uint32_t n = 0;
    bool prev = false;
    while (p < len) {
        uint32_t c = s[p];
        uint32_t d = hexval(c);
        if (d < radix) {
            if (acc != nullptr) {
                embi_jsnum_digit(acc, d);
            }
            if (ev != nullptr && *ev < 100000000000ull) {
                *ev = *ev * 10 + d;
            }
            n++;
            p++;
            prev = true;
            continue;
        }
        if (c == '_' && allow_sep) {
            if (!prev || p + 1 >= len || hexval(s[p + 1]) >= radix) {
                return fail(lx, "invalid numeric separator", p);
            }
            prev = false;
            p++;
            continue;
        }
        break;
    }
    *pp = p;
    *count = n;
    return true;
}

static bool scan_number(embi_jslex *lx, embi_jstok *t, uint32_t start)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    uint32_t p = start;
    uint32_t count = 0;
    embi_jsnum acc;

    if (s[p] == '0' && p + 1 < len && s[p + 1] == '_') {
        return fail(lx, "numeric separator after a leading zero", p + 1);
    }
    if (s[p] == '0' && p + 1 < len) {
        uint32_t x = s[p + 1] | 0x20u;
        unsigned radix = x == 'x' ? 16u : x == 'o' ? 8u : x == 'b' ? 2u : 0u;
        if (radix != 0) {
            p += 2;
            embi_jsnum_init(&acc, radix);
            if (!scan_digits(lx, &p, radix, true, &acc, &count, nullptr)) {
                return false;
            }
            if (count == 0) {
                return fail(lx, "missing digits after radix prefix", start);
            }
            t->radix = (uint8_t)radix;
            if (p < len && s[p] == 'n') {
                p++;
                t->kind = EMBI_JST_BIGINT;
            } else {
                t->kind = EMBI_JST_NUMBER;
                t->num = embi_jsnum_finish(&acc, 0);
            }
            t->end = p;
            return numeric_end(lx, p);
        }
    }

    uint8_t flags = 0;
    if (s[p] == '0' && p + 1 < len && is_dec(s[p + 1])) {
        /* LegacyOctalIntegerLiteral when every digit is 0-7, else NonOctalDecimalIntegerLiteral. */
        uint32_t q = p + 1;
        bool octal = true;
        while (q < len && is_dec(s[q])) {
            if (s[q] >= '8') {
                octal = false;
            }
            q++;
        }
        if (octal) {
            embi_jsnum_init(&acc, 8);
            for (uint32_t i = p; i < q; i++) {
                embi_jsnum_digit(&acc, (unsigned)(s[i] - '0'));
            }
            t->kind = EMBI_JST_NUMBER;
            t->radix = 8;
            t->num = embi_jsnum_finish(&acc, 0);
            t->flags |= EMBI_JST_F_LEGACY;
            t->end = q;
            return numeric_end(lx, q);
        }
        flags |= EMBI_JST_F_LEGACY;
    }
    const bool legacy = (flags & EMBI_JST_F_LEGACY) != 0;

    embi_jsnum_init(&acc, 10);
    bool point = false;
    bool expo = false;
    uint64_t ev = 0;
    bool eneg = false;
    if (!scan_digits(lx, &p, 10, !legacy, &acc, &count, nullptr)) {
        return false;
    }
    if (p < len && s[p] == '.') {
        point = true;
        embi_jsnum_point(&acc);
        p++;
        if (!scan_digits(lx, &p, 10, !legacy, &acc, &count, nullptr)) {
            return false;
        }
    }
    if (p < len && (s[p] | 0x20u) == 'e') {
        expo = true;
        p++;
        if (p < len && (s[p] == '+' || s[p] == '-')) {
            eneg = s[p] == '-';
            p++;
        }
        uint32_t ecount = 0;
        if (!scan_digits(lx, &p, 10, !legacy, nullptr, &ecount, &ev)) {
            return false;
        }
        if (ecount == 0) {
            return fail(lx, "missing exponent in numeric literal", p);
        }
    }
    if (!point && !expo && !legacy && p < len && s[p] == 'n') {
        p++;
        t->kind = EMBI_JST_BIGINT;
        t->radix = 10;
        t->end = p;
        return numeric_end(lx, p);
    }
    t->kind = EMBI_JST_NUMBER;
    t->radix = 10;
    t->flags |= flags;
    t->num = embi_jsnum_finish(&acc, eneg ? -(int64_t)ev : (int64_t)ev);
    t->end = p;
    return numeric_end(lx, p);
}

/* ------------------------------------------------------------------ strings and templates */

/* p is the index just after the backslash. Appends the cooked value to the scratch buffer.
 * In a template, an invalid escape sets *bad and is skipped over; in a string it is an error. */
static bool scan_escape(embi_jslex *lx, uint32_t *pp, uint32_t *n, bool tpl, bool *bad,
                        uint8_t *flags)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    const uint32_t at = *pp;
    uint32_t p = at;
    uint32_t cp = 0;
    if (p >= len) {
        return fail(lx, "unterminated escape sequence", p);
    }
    uint32_t c = s[p];
    switch (c) {
    case 'b':
        cp = 0x08;
        p++;
        break;
    case 't':
        cp = 0x09;
        p++;
        break;
    case 'n':
        cp = 0x0A;
        p++;
        break;
    case 'v':
        cp = 0x0B;
        p++;
        break;
    case 'f':
        cp = 0x0C;
        p++;
        break;
    case 'r':
        cp = 0x0D;
        p++;
        break;
    case 'x':
        if (p + 2 < len && hexval(s[p + 1]) < 16u && hexval(s[p + 2]) < 16u) {
            cp = hexval(s[p + 1]) * 16u + hexval(s[p + 2]);
            p += 3;
            break;
        }
        goto bad_escape;
    case 'u': {
        uint32_t end;
        if (!read_u(lx, p, &cp, &end)) {
            goto bad_escape;
        }
        p = end;
        break;
    }
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        if (c == '0' && !(p + 1 < len && is_dec(s[p + 1]))) {
            cp = 0;
            p++;
            break;
        }
        if (tpl) {
            goto bad_escape;
        }
        *flags |= EMBI_JST_F_LEGACY;
        {
            uint32_t v = c - '0';
            p++;
            if (p < len && is_oct(s[p])) {
                v = v * 8 + (s[p] - '0');
                p++;
                if (c <= '3' && p < len && is_oct(s[p])) {
                    v = v * 8 + (s[p] - '0');
                    p++;
                }
            }
            cp = v;
        }
        break;
    case '8':
    case '9':
        if (tpl) {
            goto bad_escape;
        }
        *flags |= EMBI_JST_F_LEGACY;
        cp = c;
        p++;
        break;
    case '\r':
        p++;
        if (p < len && s[p] == '\n') {
            p++;
        }
        cp = NOCP;
        break;
    case '\n':
        p++;
        cp = NOCP;
        break;
    default: {
        uint32_t w;
        cp = decode_at(lx, p, &w);
        p += w;
        if (cp == 0x2028 || cp == 0x2029) {
            cp = NOCP; /* line continuation */
        }
        break;
    }
    }
    if (cp != NOCP && !put_cp(lx, n, cp)) {
        return false;
    }
    *pp = p;
    return true;

bad_escape:
    if (!tpl) {
        return fail(lx, "invalid escape sequence", at);
    }
    *bad = true;
    *pp = at + 1;
    return true;
}

static bool scan_string(embi_jslex *lx, embi_jstok *t, uint32_t start)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    const uint8_t quote = s[start];
    uint32_t p = start + 1;
    uint32_t n = 0;
    uint8_t flags = 0;
    bool bad = false;
    for (;;) {
        if (p >= len) {
            return fail(lx, "unterminated string literal", start);
        }
        uint32_t c = s[p];
        if (c == quote) {
            p++;
            break;
        }
        if (c == '\\') {
            p++;
            if (!scan_escape(lx, &p, &n, false, &bad, &flags)) {
                return false;
            }
            continue;
        }
        if (c == '\n' || c == '\r') {
            return fail(lx, "unterminated string literal", p);
        }
        if (c < 0x80u) { /* plain ASCII: no decoding; a skim scan does not build the value */
            if (!lx->skim) {
                if (n >= lx->scratch_cap) {
                    return fail(lx, "token too long for the lexer scratch buffer", lx->pos);
                }
                lx->scratch[n++] = (uint16_t)c;
            }
            p++;
            continue;
        }
        uint32_t w;
        uint32_t cp = decode_at(lx, p, &w);
        if (!put_cp(lx, &n, cp)) {
            return false;
        }
        p += w;
    }
    t->kind = EMBI_JST_STRING;
    t->cooked = lx->scratch;
    t->cooked_len = n;
    t->end = p;
    t->flags |= flags;
    return true;
}

/* p is the index just after the opening '`' or the closing '}' of a substitution. */
static bool scan_template(embi_jslex *lx, embi_jstok *t, uint32_t p, bool from_brace)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    uint32_t n = 0;
    uint32_t raw_end = 0;
    uint8_t flags = 0;
    bool bad = false;
    embi_jstok_kind kind = EMBI_JST_TPL_NOSUB;
    for (;;) {
        if (p >= len) {
            return fail(lx, "unterminated template literal", p);
        }
        uint32_t c = s[p];
        if (c == '`') {
            raw_end = p;
            p++;
            kind = from_brace ? EMBI_JST_TPL_TAIL : EMBI_JST_TPL_NOSUB;
            break;
        }
        if (c == '$' && p + 1 < len && s[p + 1] == '{') {
            raw_end = p;
            p += 2;
            kind = from_brace ? EMBI_JST_TPL_MIDDLE : EMBI_JST_TPL_HEAD;
            break;
        }
        if (c == '\\') {
            p++;
            if (p >= len) {
                return fail(lx, "unterminated template literal", p);
            }
            if (!scan_escape(lx, &p, &n, true, &bad, &flags)) {
                return false;
            }
            continue;
        }
        if (c == '\r') {
            p++;
            if (p < len && s[p] == '\n') {
                p++;
            }
            if (!put(lx, &n, 0x0A)) {
                return false;
            }
            continue;
        }
        uint32_t w;
        uint32_t cp = decode_at(lx, p, &w);
        if (!put_cp(lx, &n, cp)) {
            return false;
        }
        p += w;
    }
    t->kind = kind;
    t->aux_end = raw_end;
    t->end = p;
    if (bad) {
        t->flags |= EMBI_JST_F_BAD_ESCAPE;
        t->cooked = nullptr;
        t->cooked_len = 0;
    } else {
        t->cooked = lx->scratch;
        t->cooked_len = n;
    }
    return true;
}

/* ------------------------------------------------------------------ regular expressions */

static bool scan_regex(embi_jslex *lx, embi_jstok *t, uint32_t start)
{
    const uint32_t len = lx->len;
    uint32_t p = start + 1;
    bool in_class = false;
    for (;;) {
        if (p >= len) {
            return fail(lx, "unterminated regular expression", start);
        }
        uint32_t w;
        uint32_t cp = decode_at(lx, p, &w);
        if (is_lt(cp)) {
            return fail(lx, "unterminated regular expression", start);
        }
        if (cp == '\\') {
            p += w;
            if (p >= len) {
                return fail(lx, "unterminated regular expression", start);
            }
            cp = decode_at(lx, p, &w);
            if (is_lt(cp)) {
                return fail(lx, "unterminated regular expression", start);
            }
            p += w;
            continue;
        }
        if (cp == '[') {
            in_class = true;
        } else if (cp == ']') {
            in_class = false;
        } else if (cp == '/' && !in_class) {
            break;
        }
        p += w;
    }
    const uint32_t body_end = p;
    p++; /* closing slash */

    /* Flags: d g i m s u v y, each at most once, and not both u and v. */
    uint32_t seen = 0;
    while (p < len) {
        uint32_t w;
        uint32_t cp = decode_at(lx, p, &w);
        if (cp == '\\') {
            return fail(lx, "escape sequence in regular expression flags", p);
        }
        if (!id_part_cp(cp)) {
            break;
        }
        uint32_t bit;
        switch (cp) {
        case 'd': bit = 0; break;
        case 'g': bit = 1; break;
        case 'i': bit = 2; break;
        case 'm': bit = 3; break;
        case 's': bit = 4; break;
        case 'u': bit = 5; break;
        case 'v': bit = 6; break;
        case 'y': bit = 7; break;
        default:
            return fail(lx, "invalid regular expression flag", p);
        }
        if ((seen >> bit) & 1u) {
            return fail(lx, "duplicate regular expression flag", p);
        }
        seen |= 1u << bit;
        p += w;
    }
    if (((seen >> 5) & 1u) && ((seen >> 6) & 1u)) {
        return fail(lx, "regular expression flags u and v cannot be combined", start);
    }
    t->kind = EMBI_JST_REGEX;
    t->aux_end = body_end;
    t->end = p;
    return true;
}

/* ------------------------------------------------------------------ punctuators */

static bool scan_punct(embi_jslex *lx, embi_jstok *t, uint32_t p)
{
    const uint8_t *s = lx->src;
    const uint32_t len = lx->len;
    const uint32_t c1 = p + 1 < len ? s[p + 1] : 0u;
    const uint32_t c2 = p + 2 < len ? s[p + 2] : 0u;
    const uint32_t c3 = p + 3 < len ? s[p + 3] : 0u;
    embi_jstok_kind k = EMBI_JST_ERROR;
    uint32_t n = 1;
    switch (s[p]) {
    case '{': k = EMBI_JST_LBRACE; break;
    case '}': k = EMBI_JST_RBRACE; break;
    case '(': k = EMBI_JST_LPAREN; break;
    case ')': k = EMBI_JST_RPAREN; break;
    case '[': k = EMBI_JST_LBRACK; break;
    case ']': k = EMBI_JST_RBRACK; break;
    case ';': k = EMBI_JST_SEMI; break;
    case ',': k = EMBI_JST_COMMA; break;
    case ':': k = EMBI_JST_COLON; break;
    case '~': k = EMBI_JST_BNOT; break;
    case '.':
        if (c1 == '.' && c2 == '.') {
            k = EMBI_JST_ELLIPSIS;
            n = 3;
        } else {
            k = EMBI_JST_DOT;
        }
        break;
    case '?':
        if (c1 == '?') {
            if (c2 == '=') {
                k = EMBI_JST_NULLISH_EQ;
                n = 3;
            } else {
                k = EMBI_JST_NULLISH;
                n = 2;
            }
        } else if (c1 == '.' && !is_dec(c2)) {
            k = EMBI_JST_OPTCHAIN;
            n = 2;
        } else {
            k = EMBI_JST_QUESTION;
        }
        break;
    case '<':
        if (c1 == '<') {
            if (c2 == '=') {
                k = EMBI_JST_SHL_EQ;
                n = 3;
            } else {
                k = EMBI_JST_SHL;
                n = 2;
            }
        } else if (c1 == '=') {
            k = EMBI_JST_LE;
            n = 2;
        } else {
            k = EMBI_JST_LT;
        }
        break;
    case '>':
        if (c1 == '>') {
            if (c2 == '>') {
                if (c3 == '=') {
                    k = EMBI_JST_SHR_EQ;
                    n = 4;
                } else {
                    k = EMBI_JST_SHR;
                    n = 3;
                }
            } else if (c2 == '=') {
                k = EMBI_JST_SAR_EQ;
                n = 3;
            } else {
                k = EMBI_JST_SAR;
                n = 2;
            }
        } else if (c1 == '=') {
            k = EMBI_JST_GE;
            n = 2;
        } else {
            k = EMBI_JST_GT;
        }
        break;
    case '=':
        if (c1 == '=') {
            if (c2 == '=') {
                k = EMBI_JST_SEQ;
                n = 3;
            } else {
                k = EMBI_JST_EQ;
                n = 2;
            }
        } else if (c1 == '>') {
            k = EMBI_JST_ARROW;
            n = 2;
        } else {
            k = EMBI_JST_ASSIGN;
        }
        break;
    case '!':
        if (c1 == '=') {
            if (c2 == '=') {
                k = EMBI_JST_SNE;
                n = 3;
            } else {
                k = EMBI_JST_NE;
                n = 2;
            }
        } else {
            k = EMBI_JST_NOT;
        }
        break;
    case '+':
        if (c1 == '+') {
            k = EMBI_JST_INC;
            n = 2;
        } else if (c1 == '=') {
            k = EMBI_JST_ADD_EQ;
            n = 2;
        } else {
            k = EMBI_JST_ADD;
        }
        break;
    case '-':
        if (c1 == '-') {
            k = EMBI_JST_DEC;
            n = 2;
        } else if (c1 == '=') {
            k = EMBI_JST_SUB_EQ;
            n = 2;
        } else {
            k = EMBI_JST_SUB;
        }
        break;
    case '*':
        if (c1 == '*') {
            if (c2 == '=') {
                k = EMBI_JST_EXP_EQ;
                n = 3;
            } else {
                k = EMBI_JST_EXP;
                n = 2;
            }
        } else if (c1 == '=') {
            k = EMBI_JST_MUL_EQ;
            n = 2;
        } else {
            k = EMBI_JST_MUL;
        }
        break;
    case '%':
        if (c1 == '=') {
            k = EMBI_JST_MOD_EQ;
            n = 2;
        } else {
            k = EMBI_JST_MOD;
        }
        break;
    case '&':
        if (c1 == '&') {
            if (c2 == '=') {
                k = EMBI_JST_LAND_EQ;
                n = 3;
            } else {
                k = EMBI_JST_LAND;
                n = 2;
            }
        } else if (c1 == '=') {
            k = EMBI_JST_BAND_EQ;
            n = 2;
        } else {
            k = EMBI_JST_BAND;
        }
        break;
    case '|':
        if (c1 == '|') {
            if (c2 == '=') {
                k = EMBI_JST_LOR_EQ;
                n = 3;
            } else {
                k = EMBI_JST_LOR;
                n = 2;
            }
        } else if (c1 == '=') {
            k = EMBI_JST_BOR_EQ;
            n = 2;
        } else {
            k = EMBI_JST_BOR;
        }
        break;
    case '^':
        if (c1 == '=') {
            k = EMBI_JST_BXOR_EQ;
            n = 2;
        } else {
            k = EMBI_JST_BXOR;
        }
        break;
    case '/':
        if (c1 == '=') {
            k = EMBI_JST_DIV_EQ;
            n = 2;
        } else {
            k = EMBI_JST_DIV;
        }
        break;
    default:
        return fail(lx, "invalid character", p);
    }
    t->kind = k;
    t->end = p + n;
    return true;
}

/* ------------------------------------------------------------------ token driver */

static void tok_init(embi_jstok *t)
{
    t->kind = EMBI_JST_EOF;
    t->kw = EMBI_JSKW_NONE;
    t->radix = 10;
    t->flags = 0;
    t->start = 0;
    t->end = 0;
    t->aux_end = 0;
    t->line = 0;
    t->col = 0;
    t->num = 0;
    t->cooked = nullptr;
    t->cooked_len = 0;
}

/* Clears the value fields of a token that is re-scanned in place. Keeps start and NL flag. */
static void tok_reset_value(embi_jstok *t)
{
    t->flags &= EMBI_JST_F_NL_BEFORE;
    t->kw = EMBI_JSKW_NONE;
    t->radix = 10;
    t->aux_end = 0;
    t->num = 0;
    t->cooked = nullptr;
    t->cooked_len = 0;
}

static void error_token(embi_jslex *lx, embi_jstok *t)
{
    lx->failed = true;
    lx->pos = lx->len;
    t->kind = EMBI_JST_ERROR;
    t->start = lx->err_pos;
    t->end = lx->err_pos;
    t->kw = EMBI_JSKW_NONE;
    t->cooked = nullptr;
    t->cooked_len = 0;
    pos_linecol(lx, t->start, &t->line, &t->col);
}

void embi_jslex_init(embi_jslex *lx, const uint8_t *src, uint32_t len, uint16_t *scratch,
                     uint32_t scratch_cap, uint32_t flags)
{
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    lx->flags = flags;
    lx->scratch = scratch;
    lx->scratch_cap = scratch_cap;
    lx->regex_ok = false;
    lx->skim = false;
    lx->failed = false;
    lx->err = nullptr;
    lx->err_pos = 0;
    lx->pc_pos = 0;
    lx->pc_line = 1;
    lx->pc_col = 0;
}

void embi_jslex_next(embi_jslex *lx, embi_jstok *t)
{
    const bool regex_ok = lx->regex_ok;
    lx->regex_ok = false;
    tok_init(t);
    if (lx->failed) {
        error_token(lx, t);
        return;
    }

    uint32_t pos = lx->pos;
    bool nl = false;
    if (!skip_trivia(lx, &pos, &nl)) {
        error_token(lx, t);
        return;
    }
    t->start = pos;
    if (nl) {
        t->flags |= EMBI_JST_F_NL_BEFORE;
    }
    if (!lx->skim) {
        pos_linecol(lx, pos, &t->line, &t->col);
    }
    if (pos >= lx->len) {
        t->kind = EMBI_JST_EOF;
        t->end = pos;
        lx->pos = pos;
        return;
    }

    const uint8_t c = lx->src[pos];
    bool ok;
    if (c == '"' || c == '\'') {
        ok = scan_string(lx, t, pos);
    } else if (c == '`') {
        ok = scan_template(lx, t, pos + 1, false);
    } else if (c == '#') {
        ok = scan_name(lx, t, pos + 1, true);
    } else if (c == '/' && regex_ok) {
        ok = scan_regex(lx, t, pos);
    } else if (is_dec(c) || (c == '.' && pos + 1 < lx->len && is_dec(lx->src[pos + 1]))) {
        ok = scan_number(lx, t, pos);
    } else if (c == '$' || c == '_' || c == '\\' || ((c | 0x20u) - 'a') < 26u) {
        ok = scan_name(lx, t, pos, false);
    } else if (c >= 0x80) {
        uint32_t w;
        uint32_t cp = decode_at(lx, pos, &w);
        ok = id_start_cp(cp) ? scan_name(lx, t, pos, false) : fail(lx, "invalid character", pos);
    } else {
        ok = scan_punct(lx, t, pos);
    }
    if (!ok) {
        error_token(lx, t);
        return;
    }
    lx->pos = t->end;
}

void embi_jslex_rescan_regex(embi_jslex *lx, embi_jstok *t)
{
    lx->regex_ok = false;
    tok_reset_value(t);
    if (lx->failed) {
        error_token(lx, t);
        return;
    }
    if (scan_regex(lx, t, t->start)) {
        lx->pos = t->end;
    } else {
        error_token(lx, t);
    }
}

void embi_jslex_rescan_template(embi_jslex *lx, embi_jstok *t)
{
    lx->regex_ok = false;
    tok_reset_value(t);
    if (lx->failed) {
        error_token(lx, t);
        return;
    }
    if (scan_template(lx, t, t->start + 1, true)) {
        lx->pos = t->end;
    } else {
        error_token(lx, t);
    }
}

uint32_t embi_jslex_raw16(const embi_jslex *lx, const embi_jstok *t, uint16_t *out, uint32_t cap)
{
    uint32_t p = t->start + 1;
    const uint32_t stop = t->aux_end;
    uint32_t n = 0;
    while (p < stop) {
        uint32_t cp;
        if (lx->src[p] == '\r') {
            cp = 0x0A;
            p++;
            if (p < stop && lx->src[p] == '\n') {
                p++;
            }
        } else {
            uint32_t w;
            cp = decode_at(lx, p, &w);
            p += w;
        }
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            if (n < cap) {
                out[n] = (uint16_t)(0xD800u + (cp >> 10));
            }
            n++;
            if (n < cap) {
                out[n] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
            }
            n++;
        } else {
            if (n < cap) {
                out[n] = (uint16_t)cp;
            }
            n++;
        }
    }
    return n;
}

uint32_t embi_jslex_bigint_digits(const embi_jslex *lx, const embi_jstok *t, char *out,
                                  uint32_t cap)
{
    uint32_t p = t->start;
    const uint32_t stop = t->end - 1; /* the 'n' suffix */
    if (t->radix != 10) {
        p += 2;
    }
    uint32_t n = 0;
    for (; p < stop; p++) {
        uint8_t c = lx->src[p];
        if (c == '_') {
            continue;
        }
        if (n < cap) {
            out[n] = (char)c;
        }
        n++;
    }
    return n;
}

embi_jskw embi_jslex_keyword(const uint8_t *p, uint32_t len)
{
    /* FP1a: a switch on the first byte, then the few candidates, instead of a scan of the whole list.
     * test_jslex_tokens checks every word of EMBI_JSKW_LIST. */
    if (len < 2 || len > 10) {
        return EMBI_JSKW_NONE;
    }
#define KW_IS(k) (len == sizeof(#k) - 1u && memcmp(p, #k, len) == 0)
#define KW_TRY(k) if (KW_IS(k)) { return EMBI_JSKW_##k; }
    switch (p[0]) {
    case 'a':
        KW_TRY(as)
        KW_TRY(async)
        KW_TRY(await)
        break;
    case 'b':
        KW_TRY(break)
        break;
    case 'c':
        KW_TRY(case)
        KW_TRY(catch)
        KW_TRY(class)
        KW_TRY(const)
        KW_TRY(continue)
        break;
    case 'd':
        KW_TRY(debugger)
        KW_TRY(default)
        KW_TRY(delete)
        KW_TRY(do)
        break;
    case 'e':
        KW_TRY(else)
        KW_TRY(enum)
        KW_TRY(export)
        KW_TRY(extends)
        break;
    case 'f':
        KW_TRY(false)
        KW_TRY(finally)
        KW_TRY(for)
        KW_TRY(function)
        KW_TRY(from)
        break;
    case 'g':
        KW_TRY(get)
        break;
    case 'i':
        KW_TRY(if)
        KW_TRY(import)
        KW_TRY(in)
        KW_TRY(instanceof)
        KW_TRY(implements)
        KW_TRY(interface)
        break;
    case 'l':
        KW_TRY(let)
        break;
    case 'm':
        KW_TRY(meta)
        break;
    case 'n':
        KW_TRY(new)
        KW_TRY(null)
        break;
    case 'o':
        KW_TRY(of)
        break;
    case 'p':
        KW_TRY(package)
        KW_TRY(private)
        KW_TRY(protected)
        KW_TRY(public)
        break;
    case 'r':
        KW_TRY(return)
        break;
    case 's':
        KW_TRY(super)
        KW_TRY(switch)
        KW_TRY(set)
        KW_TRY(static)
        break;
    case 't':
        KW_TRY(this)
        KW_TRY(throw)
        KW_TRY(true)
        KW_TRY(try)
        KW_TRY(typeof)
        KW_TRY(target)
        break;
    case 'v':
        KW_TRY(var)
        KW_TRY(void)
        break;
    case 'w':
        KW_TRY(while)
        KW_TRY(with)
        break;
    case 'y':
        KW_TRY(yield)
        break;
    default:
        break;
    }
#undef KW_TRY
#undef KW_IS
    return EMBI_JSKW_NONE;
}

static bool in_ranges(const uint32_t (*tab)[2], uint32_t n, uint32_t cp)
{
    uint32_t lo = 0;
    uint32_t hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (tab[mid][0] <= cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    return cp <= tab[lo - 1][1];
}

bool embi_jslex_is_id_start(uint32_t cp)
{
    if (cp < 0x80) {
        return ((cp | 0x20u) - 'a') < 26u;
    }
    return in_ranges(embi_jslex_id_start_tab,
                     (uint32_t)(sizeof embi_jslex_id_start_tab / sizeof embi_jslex_id_start_tab[0]),
                     cp);
}

bool embi_jslex_is_id_continue(uint32_t cp)
{
    if (cp < 0x80) {
        return (((cp | 0x20u) - 'a') < 26u) || (cp - '0' < 10u) || cp == '_';
    }
    return in_ranges(
        embi_jslex_id_continue_tab,
        (uint32_t)(sizeof embi_jslex_id_continue_tab / sizeof embi_jslex_id_continue_tab[0]), cp);
}
