/*
 * HTML tokenizer, WHATWG HTML 13.2.5. The contract is in tokenizer.h.
 *
 * Each state is one case in step(). A step consumes input, changes state, or both. When a step
 * needs input that has not arrived yet, it returns false and nothing changes, so the next run
 * repeats the step. "Reconsume" is a state change without consumption: the next step peeks the same
 * character again. A character is consumed (adv) only after the step has decided what to do with it.
 *
 * Error positions use the index of the next unconsumed character. A parse error about a character
 * that the spec has already consumed is therefore reported before adv(), so its column is that
 * character's column. Errors reported after a '>' that has been consumed use err_back(1).
 */

#include "html/tokenizer.h"
#include "html/names.h"

#include <string.h>

#define HT_EOF 0xFFFFFFFFu
#define HT_REF_MAX 40
#define HT_TEXT_FLUSH 65536u
#define HT_FEED_SLICE 4096u

/* Empties a string builder (embi_sb_clear, inlined: the tokenizer clears them per token). */
#define sb_reset(sb) ((void)((sb)->len = 0))

/* One row of the named character reference table (entities.inc, SZ3a): 8 bytes, no pointers. The name (ASCII, with the
 * trailing ';' when the spec has one) is ht_entity_names[off .. off + len), not NUL-terminated. */
typedef struct ht_entity {
    uint16_t off;
    uint8_t len;
    uint8_t cp2i; /* index in ht_entity_cp2 of the second code point, 0 for none */
    uint32_t cp1;
} ht_entity;

#include "entities.inc"

static inline const char *ht_ent_name(size_t i)
{
    return ht_entity_names + ht_entities[i].off;
}

#define HT_ENTITY_COUNT (sizeof ht_entities / sizeof ht_entities[0])

typedef enum ht_state {
    HS_DATA,
    HS_RCDATA,
    HS_RAWTEXT,
    HS_SCRIPT_DATA,
    HS_PLAINTEXT,
    HS_TAG_OPEN,
    HS_END_TAG_OPEN,
    HS_TAG_NAME,
    HS_RCDATA_LT,
    HS_RCDATA_END_OPEN,
    HS_RCDATA_END_NAME,
    HS_RAWTEXT_LT,
    HS_RAWTEXT_END_OPEN,
    HS_RAWTEXT_END_NAME,
    HS_SCRIPT_LT,
    HS_SCRIPT_END_OPEN,
    HS_SCRIPT_END_NAME,
    HS_SCRIPT_ESC_START,
    HS_SCRIPT_ESC_START_DASH,
    HS_SCRIPT_ESCAPED,
    HS_SCRIPT_ESCAPED_DASH,
    HS_SCRIPT_ESCAPED_DASH_DASH,
    HS_SCRIPT_ESCAPED_LT,
    HS_SCRIPT_ESCAPED_END_OPEN,
    HS_SCRIPT_ESCAPED_END_NAME,
    HS_SCRIPT_DBL_START,
    HS_SCRIPT_DBL_ESCAPED,
    HS_SCRIPT_DBL_ESCAPED_DASH,
    HS_SCRIPT_DBL_ESCAPED_DASH_DASH,
    HS_SCRIPT_DBL_ESCAPED_LT,
    HS_SCRIPT_DBL_END,
    HS_BEFORE_ATTR_NAME,
    HS_ATTR_NAME,
    HS_AFTER_ATTR_NAME,
    HS_BEFORE_ATTR_VALUE,
    HS_ATTR_VALUE_DQ,
    HS_ATTR_VALUE_SQ,
    HS_ATTR_VALUE_UQ,
    HS_AFTER_ATTR_VALUE_QUOTED,
    HS_SELF_CLOSING_START,
    HS_BOGUS_COMMENT,
    HS_PI_OPEN,
    HS_PI_TARGET,
    HS_PI_BEFORE_DATA,
    HS_PI_DATA,
    HS_PI_QUESTION,
    HS_MARKUP_DECL_OPEN,
    HS_COMMENT_START,
    HS_COMMENT_START_DASH,
    HS_COMMENT,
    HS_COMMENT_LT,
    HS_COMMENT_LT_BANG,
    HS_COMMENT_LT_BANG_DASH,
    HS_COMMENT_LT_BANG_DASH_DASH,
    HS_COMMENT_END_DASH,
    HS_COMMENT_END,
    HS_COMMENT_END_BANG,
    HS_DOCTYPE,
    HS_BEFORE_DOCTYPE_NAME,
    HS_DOCTYPE_NAME,
    HS_AFTER_DOCTYPE_NAME,
    HS_AFTER_DOCTYPE_PUBLIC_KW,
    HS_BEFORE_DOCTYPE_PUBLIC,
    HS_DOCTYPE_PUBLIC_DQ,
    HS_DOCTYPE_PUBLIC_SQ,
    HS_AFTER_DOCTYPE_PUBLIC,
    HS_BETWEEN_DOCTYPE_IDS,
    HS_AFTER_DOCTYPE_SYSTEM_KW,
    HS_BEFORE_DOCTYPE_SYSTEM,
    HS_DOCTYPE_SYSTEM_DQ,
    HS_DOCTYPE_SYSTEM_SQ,
    HS_AFTER_DOCTYPE_SYSTEM,
    HS_BOGUS_DOCTYPE,
    HS_CDATA,
    HS_CDATA_BRACKET,
    HS_CDATA_END,
    HS_CHAR_REF,
    HS_NAMED_REF,
    HS_AMBIGUOUS_AMP,
    HS_NUMERIC_REF,
    HS_HEX_START,
    HS_DEC_START,
    HS_HEX_REF,
    HS_DEC_REF,
    HS_NUMERIC_END,
} ht_state;

typedef struct ht_attr_slot {
    embi_sb name;
    embi_sb value;
} ht_attr_slot;

struct embi_html_tokenizer {
    embi_html_sink_fn sink;
    embi_html_error_fn on_error;
    void *ctx;

    /* Input bytes. buf[pos..len) is unconsumed UTF-8, and index is the code point index of buf[pos]
     * in the stream. Characters are decoded when they are peeked (see dec_at), so no code point
     * array is kept. */
    uint8_t *buf;
    size_t len;
    size_t cap;
    size_t pos;
    uint64_t index;
    uint64_t checked; /* absolute index: input stream checks ran for every character before it */
    /* Unit sizes (1 or 2 UTF-16 units) of the last eight consumed characters, newest in bit 0. */
    uint8_t hist;
    bool eof;   /* end() was called */
    bool done;  /* the EOF token has been emitted */
    bool paused;

    bool at_start;
    bool strip_bom;
    bool want_atoms; /* tags from fast_tag carry atoms (embi_html_tokenizer_want_atoms) */

    /* Line and column. Columns count UTF-16 code units, as the html5lib tests do: units is the
     * absolute count consumed so far, and line_units is its value at the start of the line. */
    uint32_t line;
    uint64_t units;
    uint64_t line_units;

    ht_state state;
    ht_state ret;
    bool cdata_allowed;

    embi_sb text;  /* pending character run */
    embi_sb temp;  /* temporary buffer (end tag names, script escapes) */
    embi_sb comment;

    embi_sb tag_name;
    embi_sb pi_target;
    /* Where the target ended; an EOF inside the data is reported there (as the html5lib data expects). */
    uint64_t pi_off;
    uint32_t pi_line;
    uint32_t pi_col;
    bool tag_is_end;
    bool self_closing;
    ht_attr_slot *attrs; /* committed attributes are [0, attr_count); the slot after them is the one being built */
    size_t attr_count;
    size_t attr_cap;
    bool attr_active;
    bool attr_dup;
    embi_html_attr *out_attrs;
    size_t out_cap;
    embi_sb last_start;

    embi_sb dt_name;
    embi_sb dt_public;
    embi_sb dt_system;
    bool dt_has_name;
    bool dt_has_public;
    bool dt_has_system;
    bool dt_force_quirks;

    /* Character reference state. */
    uint32_t ref[HT_REF_MAX];
    size_t ref_n;
    uint32_t ref_code;

    /* First-character ranges of the entity table, built on first use (see match_named). */
    bool ent_ready;
    uint16_t ent_lo[128];
    uint16_t ent_hi[128];
};

#define PEEK(c)                                                                                \
    do {                                                                                       \
        if (!peekn(t, 0, &(c))) {                                                              \
            return false;                                                                      \
        }                                                                                      \
    } while (0)

static inline bool is_ws(uint32_t c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x20;
}

static inline bool is_upper(uint32_t c)
{
    return c >= 'A' && c <= 'Z';
}

static inline bool is_lower(uint32_t c)
{
    return c >= 'a' && c <= 'z';
}

static inline bool is_alpha(uint32_t c)
{
    return is_upper(c) || is_lower(c);
}

static inline bool is_digit(uint32_t c)
{
    return c >= '0' && c <= '9';
}

static inline bool is_hex(uint32_t c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static inline bool is_alnum(uint32_t c)
{
    return is_alpha(c) || is_digit(c);
}

static inline uint32_t to_lower(uint32_t c)
{
    return is_upper(c) ? c + 0x20 : c;
}

static inline uint32_t hex_value(uint32_t c)
{
    if (is_digit(c)) {
        return c - '0';
    }
    return (to_lower(c) - 'a') + 10;
}

static inline bool is_noncharacter(uint32_t c)
{
    return (c >= 0xFDD0 && c <= 0xFDEF) || (c <= 0x10FFFF && (c & 0xFFFE) == 0xFFFE);
}

static inline bool is_attr_ret(ht_state s)
{
    return s == HS_ATTR_VALUE_DQ || s == HS_ATTR_VALUE_SQ || s == HS_ATTR_VALUE_UQ;
}

static inline embi_str sb_str(const embi_sb *sb)
{
    return (embi_str){sb->data, sb->len};
}

static inline ht_attr_slot *cur_attr(embi_html_tokenizer *t)
{
    return &t->attrs[t->attr_count];
}

/* ------------------------------------------------------------------ input and errors */

static void check_input(embi_html_tokenizer *t, uint32_t c);

/* Decodes the character that starts at byte offset off (off < len). Returns false when the bytes
 * that the character needs are not all here yet and more input may come (eof false). nb receives
 * the number of bytes the character occupies. The decoder is the WHATWG UTF-8 decoder, except that
 * encoded surrogates are accepted (WTF-8): a bad or truncated sequence is one U+FFFD covering its
 * maximal valid prefix. CR and CR LF decode as one LF. */
static bool dec_at(const embi_html_tokenizer *t, size_t off, bool eof, uint32_t *cp, size_t *nb)
{
    const uint8_t *p = t->buf + off;
    size_t avail = t->len - off;
    uint8_t b = p[0];
    if (b < 0x80) {
        if (b != 0x0D) {
            *cp = b;
            *nb = 1;
            return true;
        }
        if (avail < 2 && !eof) {
            return false; /* a LF may follow */
        }
        *cp = 0x0A;
        *nb = (avail >= 2 && p[1] == 0x0A) ? 2 : 1;
        return true;
    }
    uint32_t c;
    size_t need;
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) {
        need = 1;
        c = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
        need = 2;
        c = b & 0x0F;
        if (b == 0xE0) {
            lo = 0xA0;
        }
    } else if (b >= 0xF0 && b <= 0xF4) {
        need = 3;
        c = b & 0x07;
        if (b == 0xF0) {
            lo = 0x90;
        }
        if (b == 0xF4) {
            hi = 0x8F;
        }
    } else {
        *cp = 0xFFFD;
        *nb = 1;
        return true;
    }
    for (size_t k = 0; k < need; k++) {
        if (1 + k >= avail) {
            if (!eof) {
                return false;
            }
            *cp = 0xFFFD;
            *nb = 1 + k;
            return true;
        }
        uint8_t x = p[1 + k];
        if (x < lo || x > hi) {
            *cp = 0xFFFD;
            *nb = 1 + k;
            return true;
        }
        lo = 0x80;
        hi = 0xBF;
        c = (c << 6) | (x & 0x3F);
    }
    *cp = c;
    *nb = 1 + need;
    return true;
}

/* Yields the character at pos+d without consuming it. HT_EOF past the end once end() was called.
 * Returns false when more input is needed. The input stream checks run when a character is first
 * read at the current position (d == 0), as the spec's "consume the next input character" does, so
 * a character that is reconsumed is not checked twice. */
static bool peek_slow(embi_html_tokenizer *t, size_t d, uint32_t *c)
{
    size_t off = t->pos;
    uint32_t cp = 0;
    size_t nb = 0;
    for (size_t k = 0;; k++) {
        if (off >= t->len) {
            if (t->eof) {
                *c = HT_EOF;
                return true;
            }
            return false;
        }
        if (!dec_at(t, off, t->eof, &cp, &nb)) {
            return false;
        }
        if (k == d) {
            break;
        }
        off += nb;
    }
    *c = cp;
    if (d == 0 && t->index >= t->checked) {
        t->checked = t->index + 1;
        /* Printable ASCII never raises an input stream error (see check_input). */
        if (cp < 0x20 || cp > 0x7E) {
            check_input(t, cp);
        }
    }
    return true;
}

[[gnu::always_inline]] static inline bool peekn(embi_html_tokenizer *t, size_t d, uint32_t *c)
{
    if (d == 0 && t->pos < t->len) {
        uint8_t b = t->buf[t->pos];
        /* Printable ASCII: no decoding and no input stream check. */
        if (b >= 0x20 && b < 0x7F) {
            *c = b;
            return true;
        }
    }
    return peek_slow(t, d, c);
}

[[gnu::always_inline]] static inline uint32_t unit_size(uint32_t c)
{
    return c >= 0x10000 ? 2 : 1;
}

/* Reports at the character "back" positions before pos (0 is the next unconsumed character). */
static void err_back(embi_html_tokenizer *t, const char *code, uint32_t back)
{
    if (t->on_error == nullptr) {
        return;
    }
    uint64_t units = t->units;
    for (uint32_t i = 0; i < back; i++) {
        units -= (t->hist >> i) & 1u ? 2u : 1u;
    }
    uint64_t off = t->index - back;
    t->on_error(t->ctx, code, off, t->line, (uint32_t)(units - t->line_units + 1));
}

static void err(embi_html_tokenizer *t, const char *code)
{
    err_back(t, code, 0);
}

static void emit_eof(embi_html_tokenizer *t);

static void pi_mark(embi_html_tokenizer *t)
{
    t->pi_off = t->index;
    t->pi_line = t->line;
    t->pi_col = (uint32_t)(t->units - t->line_units + 1);
}

static void pi_eof(embi_html_tokenizer *t)
{
    if (t->on_error != nullptr) {
        t->on_error(t->ctx, "eof-in-processing-instruction", t->pi_off, t->pi_line, t->pi_col);
    }
    t->state = HS_DATA;
    emit_eof(t);
}

static void check_input(embi_html_tokenizer *t, uint32_t c)
{
    if (c >= 0xD800 && c <= 0xDFFF) {
        err(t, "surrogate-in-input-stream");
    } else if (is_noncharacter(c)) {
        err(t, "noncharacter-in-input-stream");
    } else if ((c < 0x20 && c != 0 && !is_ws(c)) || (c >= 0x7F && c <= 0x9F)) {
        err(t, "control-character-in-input-stream");
    }
}

/* Consumes the character at pos. The caller has checked that it is not EOF. */
[[gnu::always_inline]] static inline void adv(embi_html_tokenizer *t)
{
    uint8_t b = t->buf[t->pos];
    uint32_t c = b;
    size_t nb = 1;
    if (b < 0x20 || b >= 0x7F) {
        /* The caller has peeked this character, so its bytes are all here. */
        (void)dec_at(t, t->pos, true, &c, &nb);
    }
    t->pos += nb;
    t->index++;
    uint32_t u = unit_size(c);
    t->hist = (uint8_t)((t->hist << 1) | (u == 2 ? 1u : 0u));
    t->units += u;
    if (c == 0x0A) {
        t->line++;
        t->line_units = t->units;
    }
}

/* Consumes n characters that the caller has already matched. */
static void adv_n(embi_html_tokenizer *t, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        adv(t);
    }
}

/* Returns 1 when the ASCII literal matches at pos, 0 when it does not, -1 when more input is needed.
 * Nothing is consumed. */
static int ahead_lit(embi_html_tokenizer *t, const char *lit, bool ci)
{
    for (size_t i = 0; lit[i] != '\0'; i++) {
        uint32_t c;
        if (!peekn(t, i, &c)) {
            return -1;
        }
        if (c == HT_EOF) {
            return 0;
        }
        uint32_t want = (unsigned char)lit[i];
        if (ci) {
            c = to_lower(c);
            want = to_lower(want);
        }
        if (c != want) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ bulk runs
 *
 * A bulk run consumes a run of plain ASCII characters in one pass: characters that the current
 * state would simply append, that raise no input-stream error, and that do not end the state.
 * Everything else (controls, non-ASCII, specials) takes the normal per-character path, so error
 * positions and state changes are the same as one character at a time. Line feeds are counted.
 */
typedef enum ht_bulk {
    HB_TEXT, /* data and RCDATA text: no '<' or '&' */
    HB_TAG,  /* tag name */
    HB_ATTR, /* attribute name */
    HB_DQ,   /* double-quoted attribute value: no '"' or '&' */
    HB_SQ,   /* single-quoted attribute value: no '\'' or '&' */
    HB_UQ,   /* unquoted attribute value: no white space, '&', '>', quotes, '<', '=' or '`' */
} ht_bulk;

[[gnu::always_inline]] static inline bool bulk_ok(ht_bulk kind, uint32_t c)
{
    switch (kind) {
    case HB_TEXT:
        return (c >= 0x20 && c < 0x7F && c != '<' && c != '&') || c == 0x09 || c == 0x0A ||
               c == 0x0C;
    case HB_TAG:
        return c > 0x20 && c < 0x7F && c != '/' && c != '>';
    case HB_ATTR:
        return c > 0x20 && c < 0x7F && c != '/' && c != '>' && c != '=' && c != '"' &&
               c != '\'' && c != '<';
    case HB_DQ:
        return (c >= 0x20 && c < 0x7F && c != '"' && c != '&') || c == 0x09 || c == 0x0A ||
               c == 0x0C;
    case HB_SQ:
        return (c >= 0x20 && c < 0x7F && c != '\'' && c != '&') || c == 0x09 || c == 0x0A ||
               c == 0x0C;
    case HB_UQ:
        return c > 0x20 && c < 0x7F && c != '&' && c != '>' && c != '"' && c != '\'' &&
               c != '<' && c != '=' && c != '`';
    }
    return false;
}

/* Word-at-a-time classification of 8 input bytes (portable C: a memcpy load, no unaligned access).
 * Each helper returns a mask with the high bit of a lane set exactly where the lane's byte
 * matches, so the lowest set lane is the first byte that ends a run. The lane arithmetic never
 * carries across lanes. */
#define HT_ONES 0x0101010101010101ull
#define HT_HIGHS 0x8080808080808080ull
#define HT_LOW7 0x7F7F7F7F7F7F7F7Full

/* Lanes whose byte (ignoring the high bit) is below n, for 1 <= n <= 127. */
[[gnu::always_inline]] static inline uint64_t lanes_lt(uint64_t v, uint8_t n)
{
    uint64_t ge = (v & HT_LOW7) + (uint64_t)(0x80u - n) * HT_ONES;
    return ~ge & HT_HIGHS;
}

/* Lanes whose byte equals c. */
[[gnu::always_inline]] static inline uint64_t lanes_eq(uint64_t v, uint8_t c)
{
    uint64_t x = v ^ ((uint64_t)c * HT_ONES);
    return ~((((x & HT_LOW7) + HT_LOW7) | x) | HT_LOW7);
}

/* Lanes that end a run of the given kind: the bytes bulk_ok rejects. */
[[gnu::always_inline]] static inline uint64_t bad_lanes(ht_bulk kind, uint64_t v)
{
    uint64_t bad = v & HT_HIGHS;
    /* Controls other than tab, LF and FF. */
    uint64_t ctl = lanes_lt(v, 0x20) & ~(lanes_eq(v, 0x09) | lanes_eq(v, 0x0A) | lanes_eq(v, 0x0C));
    switch (kind) {
    case HB_TEXT:
        return bad | ctl | lanes_eq(v, '<') | lanes_eq(v, '&');
    case HB_TAG:
        return bad | lanes_lt(v, 0x21) | lanes_eq(v, '/') | lanes_eq(v, '>');
    case HB_ATTR:
        return bad | lanes_lt(v, 0x21) | lanes_eq(v, '/') | lanes_eq(v, '>') | lanes_eq(v, '=') |
               lanes_eq(v, '"') | lanes_eq(v, '\'') | lanes_eq(v, '<');
    case HB_DQ:
        return bad | ctl | lanes_eq(v, '"') | lanes_eq(v, '&');
    case HB_SQ:
        return bad | ctl | lanes_eq(v, '\'') | lanes_eq(v, '&');
    case HB_UQ:
        return bad | lanes_lt(v, 0x21) | lanes_eq(v, '&') | lanes_eq(v, '>') | lanes_eq(v, '"') |
               lanes_eq(v, '\'') | lanes_eq(v, '<') | lanes_eq(v, '=') | lanes_eq(v, '`');
    }
    return bad;
}

/* Length of the plain prefix of src[0..n): the bytes that bulk_ok accepts for kind. */
[[gnu::always_inline]] static inline size_t bulk_scan(ht_bulk kind, const uint8_t *src, size_t n)
{
    size_t k = 0;
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || \
                                  __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    while (k + 8 <= n) {
        uint64_t v;
        memcpy(&v, src + k, 8);
        uint64_t m = bad_lanes(kind, v);
        if (m != 0) {
            /* The lane arithmetic does not depend on byte order. The first byte of the word is the
             * lowest lane on a little-endian target and the highest on a big-endian one. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return k + ((size_t)__builtin_ctzll(m) >> 3);
#else
            return k + ((size_t)__builtin_clzll(m) >> 3);
#endif
        }
        k += 8;
    }
#endif
    while (k < n && bulk_ok(kind, src[k])) {
        k++;
    }
    return k;
}

/* Appends the run to out (lowercasing ASCII upper case when fold is set), consumes it and returns
 * its length in bytes (0 when the next character is not a plain one). Every byte of a run is one
 * ASCII character, one UTF-16 unit, so the column count advances by the run length. Line feeds in
 * the run are counted. */
/* Consumes the k plain bytes at src (= buf + pos) that bulk_scan accepted. */
[[gnu::always_inline]] static inline void bulk_advance(embi_html_tokenizer *t, ht_bulk kind,
                                                       const uint8_t *src, size_t k)
{
    if (kind == HB_TEXT || kind == HB_DQ || kind == HB_SQ) {
        /* The only kinds that admit a line feed. */
        uint64_t units0 = t->units;
        const uint8_t *q = src;
        size_t left = k;
        while (left > 0 && (q = memchr(q, '\n', left)) != nullptr) {
            size_t j = (size_t)(q - src);
            t->line++;
            t->line_units = units0 + j + 1;
            q++;
            left = k - j - 1;
        }
    }
    t->units += k;
    t->index += k;
    t->pos += k;
    t->hist = k >= 8 ? 0 : (uint8_t)(t->hist << k);
}

[[gnu::always_inline]] static inline size_t bulk_run(embi_html_tokenizer *t, ht_bulk kind,
                                                     embi_sb *out, bool fold)
{
    const uint8_t *src = t->buf + t->pos;
    size_t n = t->len - t->pos;
    size_t k = bulk_scan(kind, src, n);
    if (k == 0) {
        return 0;
    }
    embi_vec_reserve(out, out->len + k);
    char *dst = out->data + out->len;
    if (fold) {
        for (size_t j = 0; j < k; j++) {
            uint8_t c = src[j];
            dst[j] = (char)(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
        }
    } else {
        memcpy(dst, src, k);
    }
    bulk_advance(t, kind, src, k);
    out->len += (uint32_t)k;
    return k;
}

/* ------------------------------------------------------------------ output */

/* Appends one code point as WTF-8. Surrogates are encoded as their 3-byte form. */
static void sb_cp(embi_sb *sb, uint32_t cp)
{
    if (cp < 0x80) {
        embi_sb_append_char(sb, (char)cp);
        return;
    }
    char b[4];
    size_t n;
    if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    embi_sb_append(sb, (embi_str){b, (uint32_t)n});
}

static void deliver(embi_html_tokenizer *t, const embi_html_token *tok)
{
    if (!t->sink(t->ctx, tok)) {
        t->paused = true;
    }
}

static void emit_cp(embi_html_tokenizer *t, uint32_t cp)
{
    sb_cp(&t->text, cp);
}

static void emit_lit(embi_html_tokenizer *t, const char *s)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        emit_cp(t, (unsigned char)s[i]);
    }
}

static void emit_sb_chars(embi_html_tokenizer *t, const embi_sb *sb)
{
    for (uint32_t i = 0; i < sb->len; i++) {
        emit_cp(t, (unsigned char)sb->data[i]);
    }
}

static void flush_text(embi_html_tokenizer *t)
{
    if (t->text.len == 0) {
        return;
    }
    embi_html_token tok = {0};
    tok.kind = EMBI_HTML_TOK_CHARACTERS;
    tok.data = sb_str(&t->text);
    deliver(t, &tok);
    sb_reset(&t->text);
}

static void emit_comment(embi_html_tokenizer *t)
{
    flush_text(t);
    embi_html_token tok = {0};
    tok.kind = EMBI_HTML_TOK_COMMENT;
    tok.data = sb_str(&t->comment);
    deliver(t, &tok);
    sb_reset(&t->comment);
}

/* A processing instruction token: name is the target, data the data (the comment buffer). */
static bool pi_target_is_xml(embi_str tg)
{
    static const char k2[] = "xml-stylesheet";
    size_t n = tg.len;
    if (n != 3 && n != sizeof k2 - 1) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = tg.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch + 32);
        }
        if (ch != k2[i]) {
            return false;
        }
    }
    return true;
}

static void emit_pi(embi_html_tokenizer *t)
{
    flush_text(t);
    embi_html_token tok = {0};
    tok.kind = EMBI_HTML_TOK_PI;
    tok.name = sb_str(&t->pi_target);
    tok.has_name = true;
    tok.data = sb_str(&t->comment);
    deliver(t, &tok);
    sb_reset(&t->comment);
    sb_reset(&t->pi_target);
}

static void emit_doctype(embi_html_tokenizer *t)
{
    flush_text(t);
    embi_html_token tok = {0};
    tok.kind = EMBI_HTML_TOK_DOCTYPE;
    tok.name = sb_str(&t->dt_name);
    tok.has_name = t->dt_has_name;
    tok.public_id = sb_str(&t->dt_public);
    tok.has_public_id = t->dt_has_public;
    tok.system_id = sb_str(&t->dt_system);
    tok.has_system_id = t->dt_has_system;
    tok.force_quirks = t->dt_force_quirks;
    deliver(t, &tok);
}

static void emit_eof(embi_html_tokenizer *t)
{
    flush_text(t);
    embi_html_token tok = {0};
    tok.kind = EMBI_HTML_TOK_EOF;
    deliver(t, &tok);
    t->done = true;
}

static void ensure_attr_cap(embi_html_tokenizer *t, size_t n)
{
    if (n <= t->attr_cap) {
        return;
    }
    size_t cap = t->attr_cap ? t->attr_cap * 2 : 8;
    while (cap < n) {
        cap *= 2;
    }
    size_t old_bytes = t->attr_cap * sizeof *t->attrs;
    t->attrs = embi_realloc(t->attrs, old_bytes, cap * sizeof *t->attrs);
    memset(t->attrs + t->attr_cap, 0, (cap - t->attr_cap) * sizeof *t->attrs);
    t->attr_cap = cap;
}

static void create_tag(embi_html_tokenizer *t, bool is_end)
{
    sb_reset(&t->tag_name);
    t->attr_count = 0;
    t->attr_active = false;
    t->attr_dup = false;
    t->self_closing = false;
    t->tag_is_end = is_end;
}

/* Commits the attribute being built, unless it was a duplicate. */
static void finish_attr(embi_html_tokenizer *t)
{
    if (t->attr_active && !t->attr_dup) {
        t->attr_count++;
    }
    t->attr_active = false;
    t->attr_dup = false;
}

static void start_attr(embi_html_tokenizer *t)
{
    finish_attr(t);
    ensure_attr_cap(t, t->attr_count + 1);
    sb_reset(&cur_attr(t)->name);
    sb_reset(&cur_attr(t)->value);
    t->attr_active = true;
}

/* Called when the attribute name is complete. A duplicate name is an error and is dropped later. */
static void check_dup(embi_html_tokenizer *t)
{
    embi_str name = sb_str(&cur_attr(t)->name);
    for (size_t i = 0; i < t->attr_count; i++) {
        embi_str prev = sb_str(&t->attrs[i].name);
        if (prev.len == name.len && memcmp(prev.ptr, name.ptr, name.len) == 0) {
            err(t, "duplicate-attribute");
            t->attr_dup = true;
            return;
        }
    }
}

static void emit_tag(embi_html_tokenizer *t, uint32_t back)
{
    flush_text(t);
    finish_attr(t);
    if (t->tag_is_end) {
        if (t->attr_count > 0) {
            err_back(t, "end-tag-with-attributes", back);
        }
        if (t->self_closing) {
            err_back(t, "end-tag-with-trailing-solidus", back);
        }
    } else {
        sb_reset(&t->last_start);
        if (t->tag_name.len > 0) {
            embi_sb_append(&t->last_start, sb_str(&t->tag_name));
        }
    }
    if (t->out_cap < t->attr_count) {
        size_t cap = t->attr_count < 8 ? 8 : t->attr_count;
        t->out_attrs = embi_realloc(t->out_attrs, t->out_cap * sizeof *t->out_attrs,
                                    cap * sizeof *t->out_attrs);
        t->out_cap = cap;
    }
    for (size_t i = 0; i < t->attr_count; i++) {
        t->out_attrs[i].name = sb_str(&t->attrs[i].name);
        t->out_attrs[i].value = sb_str(&t->attrs[i].value);
        t->out_attrs[i].atom = 0;
    }
    embi_html_token tok = {0};
    tok.kind = t->tag_is_end ? EMBI_HTML_TOK_END_TAG : EMBI_HTML_TOK_START_TAG;
    tok.name = sb_str(&t->tag_name);
    tok.self_closing = t->self_closing;
    tok.attrs = t->out_attrs;
    tok.attr_count = (uint32_t)t->attr_count;
    deliver(t, &tok);
}

static bool plain_ref_cp(uint32_t v)
{
    return (v >= 0x20 && v <= 0x7E) || (v >= 0xA0 && v <= 0xD7FF) ||
           (v >= 0xE000 && v <= 0xFDCF) || (v >= 0xFDF0 && v <= 0xFFFD);
}

/* The references that are most common in markup, decided without the state machine: &amp; &lt;
 * &gt; &quot; &apos; (with the semicolon, so no longer name can match) and decimal or hex numeric
 * references to a plain code point ending in ';'. p[0] is the '&' and avail bytes are here. On
 * success returns true with the code point and the length of the reference in bytes. Otherwise
 * false: the state machine handles the reference, including every error case and the wait for
 * more input. */
static bool simple_ref(const uint8_t *p, size_t avail, uint32_t *out_cp, size_t *out_n)
{
    if (avail < 3) {
        return false;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if (p[1] == '#') {
        size_t i = 2;
        bool hex = p[i] == 'x' || p[i] == 'X';
        if (hex) {
            i++;
        }
        size_t start = i;
        uint32_t v = 0;
        while (i < avail && i - start < 7) {
            uint8_t d = p[i];
            uint32_t dv;
            if (d >= '0' && d <= '9') {
                dv = d - '0';
            } else if (hex && d >= 'a' && d <= 'f') {
                dv = d - 'a' + 10;
            } else if (hex && d >= 'A' && d <= 'F') {
                dv = d - 'A' + 10;
            } else {
                break;
            }
            v = v * (hex ? 16u : 10u) + dv;
            i++;
        }
        if (i == start || i >= avail || p[i] != ';' || i - start > 6 || !plain_ref_cp(v)) {
            return false;
        }
        cp = v;
        n = i + 1;
    } else {
        static const char names[5][8] = {"amp;", "lt;", "gt;", "quot;", "apos;"};
        static const uint8_t lens[5] = {4, 3, 3, 5, 5};
        static const uint8_t outs[5] = {'&', '<', '>', '"', '\''};
        for (size_t k = 0; k < 5; k++) {
            if (avail >= 1u + lens[k] && memcmp(p + 1, names[k], lens[k]) == 0) {
                cp = outs[k];
                n = 1 + lens[k];
                break;
            }
        }
        if (n == 0) {
            return false;
        }
    }
    *out_cp = cp;
    *out_n = n;
    return true;
}

/* Fast path for those references. The caller has peeked the '&' at pos. On success the reference
 * is consumed and its character is appended for the return state ret. */
static void attr_val_put(embi_html_tokenizer *t, uint32_t cp);
static bool fast_ref(embi_html_tokenizer *t, ht_state ret)
{
    uint32_t cp = 0;
    size_t n = 0;
    if (!simple_ref(t->buf + t->pos, t->len - t->pos, &cp, &n)) {
        return false;
    }
    /* Every character of the reference is one ASCII byte (one unit, no line feed). */
    t->pos += n;
    t->index += n;
    t->units += n;
    t->hist = n >= 8 ? 0 : (uint8_t)(t->hist << n);
    if (is_attr_ret(ret)) {
        attr_val_put(t, cp);
    } else {
        emit_cp(t, cp);
    }
    return true;
}

/* A run of text in the data state: plain bytes and simple references, up to the first byte that
 * needs the state machine. Returns the number of bytes consumed. */
static size_t data_run(embi_html_tokenizer *t)
{
    size_t total = 0;
    while (t->text.len < HT_TEXT_FLUSH) {
        if (t->text.len == 0) {
            /* A plain run that a tag follows is delivered straight from the input buffer: the
             * tag will end the run anyway (a '<' and a letter, or "</" and a letter, always open
             * one), so the token is the same as the coalesced one, minus the copy. */
            const uint8_t *src = t->buf + t->pos;
            size_t avail = t->len - t->pos;
            size_t k = bulk_scan(HB_TEXT, src, avail);
            if (k > 0 && k + 2 < avail && src[k] == '<' &&
                (is_alpha(src[k + 1]) || (src[k + 1] == '/' && is_alpha(src[k + 2])))) {
                bulk_advance(t, HB_TEXT, src, k);
                embi_html_token tok = {0};
                tok.kind = EMBI_HTML_TOK_CHARACTERS;
                tok.data = (embi_str){(const char *)src, (uint32_t)k};
                deliver(t, &tok);
                return total + k;
            }
        }
        size_t k = bulk_run(t, HB_TEXT, &t->text, false);
        total += k;
        if (t->pos >= t->len || t->buf[t->pos] != '&' || !fast_ref(t, HS_DATA)) {
            break;
        }
        total++;
    }
    return total;
}

/* ------------------------------------------------------------------ fast tags
 *
 * A start or end tag that the state machine would only append characters to is parsed in one pass
 * over the buffered bytes: the name and the attributes contain nothing but printable ASCII, the
 * tag is complete in the buffer, and it raises no parse error (so no duplicate attributes, no
 * stray quotes, no "=" or "/" in odd places). Quoted attribute values may hold the simple
 * character references of fast_ref. Any other tag leaves the tokenizer untouched and goes through
 * the states, which keeps error positions and every special case in one place. The strings of the
 * token point into the input buffer (names with upper case letters and values with references are
 * built in stack buffers), so nothing is copied for the common tag.
 */
#define FT_MAX_ATTRS 16u
#define FT_NAME_SCRATCH 256u
#define FT_VALUE_SCRATCH 512u

static inline bool ft_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f';
}

/* True when one of the n bytes (all below 0x80) is in 'A'..'Z'. */
static inline bool ft_any_upper(const uint8_t *p, size_t n)
{
    size_t k = 0;
    for (; k + 8 <= n; k += 8) {
        uint64_t v;
        memcpy(&v, p + k, 8);
        uint64_t ge = v + (uint64_t)(0x80 - 'A') * HT_ONES;
        uint64_t gt = v + (uint64_t)(0x80 - 'Z' - 1) * HT_ONES;
        if ((ge & ~gt & HT_HIGHS) != 0) {
            return true;
        }
    }
    for (; k < n; k++) {
        if (p[k] >= 'A' && p[k] <= 'Z') {
            return true;
        }
    }
    return false;
}

static inline bool ft_has_byte(const uint8_t *p, size_t n, uint8_t c)
{
    size_t k = 0;
    for (; k + 8 <= n; k += 8) {
        uint64_t v;
        memcpy(&v, p + k, 8);
        if (lanes_eq(v, c) != 0) {
            return true;
        }
    }
    for (; k < n; k++) {
        if (p[k] == c) {
            return true;
        }
    }
    return false;
}

/* Copies n bytes lower case to dst. */
static inline void ft_lower(char *dst, const uint8_t *src, size_t n)
{
    for (size_t j = 0; j < n; j++) {
        uint8_t c = src[j];
        dst[j] = (char)(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
    }
}

/* Appends a code point as UTF-8 to dst (room checked by the caller: 3 bytes). */
static inline size_t ft_put_cp(char *dst, uint32_t cp)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    dst[0] = (char)(0xE0 | (cp >> 12));
    dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* Tries the tag that starts at pos (buf[pos] is '<'). Returns true when it consumed and
 * delivered the tag. Returns false, with nothing changed, when the state machine must do it. */
static bool fast_tag(embi_html_tokenizer *t)
{
    const uint8_t *b = t->buf;
    const size_t end = t->len;
    const size_t p = t->pos;
    size_t i = p + 1;
    bool is_end = false;
    if (i < end && b[i] == '/') {
        is_end = true;
        i++;
    }
    if (i >= end || !is_alpha(b[i])) {
        return false;
    }
    const size_t name_off = i;
    size_t k = bulk_scan(HB_TAG, b + i, end - i);
    if (k == 0 || k > 64 || i + k >= end) {
        return false;
    }
    const uint32_t name_len = (uint32_t)k;
    i += k;
    if (t->out_cap < FT_MAX_ATTRS) {
        t->out_attrs = embi_realloc(t->out_attrs, t->out_cap * sizeof *t->out_attrs,
                                    FT_MAX_ATTRS * sizeof *t->out_attrs);
        t->out_cap = FT_MAX_ATTRS;
    }
    embi_html_attr *out = t->out_attrs;
    uint32_t na = 0;
    bool self_closing = false;
    char nscratch[FT_NAME_SCRATCH];
    char vscratch[FT_VALUE_SCRATCH];
    size_t nused = 0;
    size_t vused = 0;
    uint8_t c = b[i];
    if (c == '>') {
        i++;
    } else if (is_end) {
        return false;
    } else if (c == '/') {
        if (i + 1 >= end || b[i + 1] != '>') {
            return false;
        }
        self_closing = true;
        i += 2;
    } else if (!ft_ws(c)) {
        return false;
    } else {
        for (;;) {
            while (i < end && ft_ws(b[i])) {
                i++;
            }
            if (i >= end) {
                return false;
            }
            c = b[i];
            if (c == '>') {
                i++;
                break;
            }
            if (c == '/') {
                if (i + 1 >= end || b[i + 1] != '>') {
                    return false;
                }
                self_closing = true;
                i += 2;
                break;
            }
            if (c == '=' || na == FT_MAX_ATTRS) {
                return false;
            }
            size_t an = bulk_scan(HB_ATTR, b + i, end - i);
            if (an == 0 || an > 64 || i + an >= end) {
                return false;
            }
            embi_html_attr *a = &out[na];
            a->name.ptr = (const char *)b + i;
            a->name.len = (uint32_t)an;
            a->value.ptr = "";
            a->value.len = 0;
            if (ft_any_upper(b + i, an)) {
                if (nused + an > FT_NAME_SCRATCH) {
                    return false;
                }
                ft_lower(nscratch + nused, b + i, an);
                a->name.ptr = nscratch + nused;
                nused += an;
            }
            i += an;
            c = b[i];
            if (!(ft_ws(c) || c == '/' || c == '>' || c == '=')) {
                return false;
            }
            if (ft_ws(c)) {
                while (i < end && ft_ws(b[i])) {
                    i++;
                }
                if (i >= end) {
                    return false;
                }
                c = b[i];
            }
            if (c == '=') {
                i++;
                while (i < end && ft_ws(b[i])) {
                    i++;
                }
                if (i >= end) {
                    return false;
                }
                c = b[i];
                if (c == '"' || c == '\'') {
                    const ht_bulk kind = c == '"' ? HB_DQ : HB_SQ;
                    i++;
                    size_t vn = bulk_scan(kind, b + i, end - i);
                    if (i + vn >= end) {
                        return false;
                    }
                    if (b[i + vn] != '&') {
                        a->value.ptr = (const char *)b + i;
                        a->value.len = (uint32_t)vn;
                        i += vn;
                    } else {
                        /* References: the value is built in the scratch buffer. */
                        char *v = vscratch + vused;
                        size_t vl = 0;
                        for (;;) {
                            if (vused + vl + vn + 3 > FT_VALUE_SCRATCH) {
                                return false;
                            }
                            memcpy(v + vl, b + i, vn);
                            vl += vn;
                            i += vn;
                            if (i >= end) {
                                return false;
                            }
                            if (b[i] != '&') {
                                break;
                            }
                            uint32_t cp = 0;
                            size_t rn = 0;
                            if (!simple_ref(b + i, end - i, &cp, &rn)) {
                                return false;
                            }
                            vl += ft_put_cp(v + vl, cp);
                            i += rn;
                            vn = bulk_scan(kind, b + i, end - i);
                        }
                        a->value.ptr = v;
                        a->value.len = (uint32_t)vl;
                        vused += vl;
                    }
                    if (b[i] != c) {
                        return false;
                    }
                    i++;
                    if (i >= end || !(ft_ws(b[i]) || b[i] == '/' || b[i] == '>')) {
                        return false;
                    }
                } else {
                    size_t vn = bulk_scan(HB_UQ, b + i, end - i);
                    if (vn == 0 || i + vn >= end || !(ft_ws(b[i + vn]) || b[i + vn] == '>')) {
                        return false;
                    }
                    a->value.ptr = (const char *)b + i;
                    a->value.len = (uint32_t)vn;
                    i += vn;
                }
            }
            /* A repeated name is an error: the state machine reports and drops it. */
            for (uint32_t j = 0; j < na; j++) {
                if (out[j].name.len == a->name.len &&
                    memcmp(out[j].name.ptr, a->name.ptr, a->name.len) == 0) {
                    return false;
                }
            }
            na++;
        }
    }

    /* Commit. Everything consumed is printable ASCII, white space (not CR) or '>': one unit each. */
    const size_t used = i - p;
    if (ft_has_byte(b + p, used, '\n')) {
        const uint8_t *q = b + p;
        size_t left = used;
        const uint8_t *nl;
        while (left > 0 && (nl = memchr(q, '\n', left)) != nullptr) {
            size_t j = (size_t)(nl - (b + p));
            t->line++;
            t->line_units = t->units + j + 1;
            q = nl + 1;
            left = used - j - 1;
        }
    }
    t->units += used;
    t->index += used;
    t->pos = i;
    t->hist = used >= 8 ? 0 : (uint8_t)(t->hist << used);

    flush_text(t);
    if (t->want_atoms) {
        for (uint32_t j = 0; j < na; j++) {
            out[j].atom = embi_html_name_atom(out[j].name.ptr, out[j].name.len, false);
        }
    } else {
        for (uint32_t j = 0; j < na; j++) {
            out[j].atom = 0;
        }
    }
    embi_html_token tok = {0};
    tok.kind = is_end ? EMBI_HTML_TOK_END_TAG : EMBI_HTML_TOK_START_TAG;
    char lname[64];
    if (ft_any_upper(b + name_off, name_len)) {
        ft_lower(lname, b + name_off, name_len);
        tok.name.ptr = lname;
    } else {
        tok.name.ptr = (const char *)b + name_off;
    }
    tok.name.len = name_len;
    if (t->want_atoms) {
        tok.has_atom = true;
        tok.name_atom = embi_html_name_atom(tok.name.ptr, name_len, false);
    }
    tok.self_closing = self_closing;
    tok.attrs = out;
    tok.attr_count = na;
    if (!is_end) {
        sb_reset(&t->last_start);
        embi_sb_append(&t->last_start, tok.name);
    }
    deliver(t, &tok);
    return true;
}

static bool appropriate_end_tag(const embi_html_tokenizer *t)
{
    return embi_str_eq(sb_str(&t->tag_name), sb_str(&t->last_start));
}

static void attr_val_put(embi_html_tokenizer *t, uint32_t cp)
{
    sb_cp(&cur_attr(t)->value, cp);
}

static void attr_name_put(embi_html_tokenizer *t, uint32_t cp)
{
    sb_cp(&cur_attr(t)->name, cp);
}

static void comment_clear(embi_html_tokenizer *t)
{
    sb_reset(&t->comment);
}

static void doctype_reset(embi_html_tokenizer *t)
{
    sb_reset(&t->dt_name);
    sb_reset(&t->dt_public);
    sb_reset(&t->dt_system);
    t->dt_has_name = false;
    t->dt_has_public = false;
    t->dt_has_system = false;
    t->dt_force_quirks = false;
}

/* ------------------------------------------------------------------ shared state fragments */

static void eof_in_tag(embi_html_tokenizer *t)
{
    err(t, "eof-in-tag");
    t->state = HS_DATA;
    emit_eof(t);
}

static bool doctype_eof(embi_html_tokenizer *t)
{
    err(t, "eof-in-doctype");
    t->dt_force_quirks = true;
    t->state = HS_DATA;
    emit_doctype(t);
    emit_eof(t);
    return true;
}

/* Code points that a numeric reference may produce without any parse error and without the
 * replacement table: printable ASCII, and the non-control, non-surrogate, non-noncharacter BMP
 * ranges. */
static void enter_ref(embi_html_tokenizer *t, ht_state ret)
{
    t->ret = ret;
    t->ref_n = 0;
    t->ref[t->ref_n++] = '&';
    t->state = HS_CHAR_REF;
}

static void ref_push(embi_html_tokenizer *t, uint32_t cp)
{
    if (t->ref_n < HT_REF_MAX) {
        t->ref[t->ref_n++] = cp;
    }
}

/* Flushes the temporary buffer to the return state: the attribute value or the text run. */
static void flush_ref(embi_html_tokenizer *t)
{
    bool attr = is_attr_ret(t->ret);
    for (size_t i = 0; i < t->ref_n; i++) {
        if (attr) {
            attr_val_put(t, t->ref[i]);
        } else {
            emit_cp(t, t->ref[i]);
        }
    }
    t->ref_n = 0;
}

/* RCDATA, RAWTEXT and script data: "<" then "/" starts an end tag candidate. */
static bool lt_state(embi_html_tokenizer *t, ht_state end_open, ht_state fallback)
{
    uint32_t c;
    PEEK(c);
    if (c == '/') {
        adv(t);
        sb_reset(&t->temp);
        t->state = end_open;
        return true;
    }
    emit_cp(t, '<');
    t->state = fallback;
    return true;
}

static bool end_open_state(embi_html_tokenizer *t, ht_state end_name, ht_state fallback)
{
    uint32_t c;
    PEEK(c);
    if (is_alpha(c)) {
        create_tag(t, true);
        t->state = end_name;
        return true;
    }
    emit_lit(t, "</");
    t->state = fallback;
    return true;
}

/* The end tag name states of RCDATA, RAWTEXT and script data. */
static bool end_name_state(embi_html_tokenizer *t, ht_state fallback)
{
    uint32_t c;
    PEEK(c);
    bool appropriate = appropriate_end_tag(t);
    if (is_ws(c) && appropriate) {
        adv(t);
        t->state = HS_BEFORE_ATTR_NAME;
        return true;
    }
    if (c == '/' && appropriate) {
        adv(t);
        t->state = HS_SELF_CLOSING_START;
        return true;
    }
    if (c == '>' && appropriate) {
        adv(t);
        t->state = HS_DATA;
        emit_tag(t, 1);
        return true;
    }
    if (is_alpha(c)) {
        adv(t);
        sb_cp(&t->tag_name, to_lower(c));
        sb_cp(&t->temp, c);
        return true;
    }
    emit_lit(t, "</");
    emit_sb_chars(t, &t->temp);
    t->state = fallback;
    return true;
}

static bool sb_is_script(const embi_sb *sb)
{
    return embi_str_eq(sb_str(sb), EMBI_STR("script"));
}

/* ------------------------------------------------------------------ character references */

/* Numeric replacement for the windows-1252 range 0x80..0x9F. Other values map to themselves. */
static uint32_t numeric_replacement(uint32_t c)
{
    switch (c) {
    case 0x80: return 0x20AC;
    case 0x82: return 0x201A;
    case 0x83: return 0x0192;
    case 0x84: return 0x201E;
    case 0x85: return 0x2026;
    case 0x86: return 0x2020;
    case 0x87: return 0x2021;
    case 0x88: return 0x02C6;
    case 0x89: return 0x2030;
    case 0x8A: return 0x0160;
    case 0x8B: return 0x2039;
    case 0x8C: return 0x0152;
    case 0x8E: return 0x017D;
    case 0x91: return 0x2018;
    case 0x92: return 0x2019;
    case 0x93: return 0x201C;
    case 0x94: return 0x201D;
    case 0x95: return 0x2022;
    case 0x96: return 0x2013;
    case 0x97: return 0x2014;
    case 0x98: return 0x02DC;
    case 0x99: return 0x2122;
    case 0x9A: return 0x0161;
    case 0x9B: return 0x203A;
    case 0x9C: return 0x0153;
    case 0x9E: return 0x017E;
    case 0x9F: return 0x0178;
    default: return c;
    }
}

/* The entity table is sorted by name, so the names that start with one character form a
 * contiguous range. ent_lo/ent_hi hold that range for each ASCII first character (empty when none). */
static void ent_index_build(embi_html_tokenizer *t)
{
    for (size_t c = 0; c < 128; c++) {
        t->ent_lo[c] = 0;
        t->ent_hi[c] = 0;
    }
    for (size_t i = 0; i < HT_ENTITY_COUNT; i++) {
        unsigned char c = (unsigned char)ht_ent_name(i)[0];
        if (c < 128) {
            if (t->ent_hi[c] == t->ent_lo[c]) {
                t->ent_lo[c] = (uint16_t)i;
            }
            t->ent_hi[c] = (uint16_t)(i + 1);
        }
    }
    t->ent_ready = true;
}

/* Longest-match search over the sorted entity table.
 * Returns -1 when more input is needed, 0 for no match, 1 for a match, and 2 for a legacy match
 * (no ';') inside an attribute that is followed by '=' or an alphanumeric. Nothing is consumed. */
static int match_named(embi_html_tokenizer *t, size_t *out_idx, uint32_t *out_len, bool attr)
{
    if (!t->ent_ready) {
        ent_index_build(t);
    }
    size_t lo = 0;
    size_t hi = HT_ENTITY_COUNT;
    {
        uint32_t c0;
        if (!peekn(t, 0, &c0)) {
            return -1;
        }
        if (c0 == HT_EOF || c0 >= 0x80) {
            return 0;
        }
        lo = t->ent_lo[c0];
        hi = t->ent_hi[c0];
        if (lo == hi) {
            return 0;
        }
    }
    long best = -1;
    uint32_t best_len = 0;
    for (uint32_t d = 0;; d++) {
        size_t first = lo;
        /* Entries sort by prefix, so an entry of length d is the first one in the range. */
        if (first < hi && ht_entities[first].len == d) {
            best = (long)first;
            best_len = d;
            first++;
        }
        if (first >= hi) {
            break;
        }
        uint32_t c;
        if (!peekn(t, d, &c)) {
            return -1;
        }
        if (c == HT_EOF || c >= 0x80) {
            break;
        }
        /* Keep the entries whose name has c at position d. */
        size_t l = first;
        size_t h = hi;
        while (l < h) {
            size_t m = l + (h - l) / 2;
            if ((uint32_t)(unsigned char)ht_ent_name(m)[d] < c) {
                l = m + 1;
            } else {
                h = m;
            }
        }
        size_t lower = l;
        h = hi;
        while (l < h) {
            size_t m = l + (h - l) / 2;
            if ((uint32_t)(unsigned char)ht_ent_name(m)[d] <= c) {
                l = m + 1;
            } else {
                h = m;
            }
        }
        if (lower == l) {
            break;
        }
        lo = lower;
        hi = l;
    }
    if (best < 0) {
        return 0;
    }
    *out_idx = (size_t)best;
    *out_len = best_len;
    if (attr && ht_ent_name((size_t)best)[best_len - 1] != ';') {
        uint32_t next;
        if (!peekn(t, best_len, &next)) {
            return -1;
        }
        if (next == '=' || is_alnum(next)) {
            return 2;
        }
    }
    return 1;
}

static uint32_t numeric_code_checked(embi_html_tokenizer *t, uint32_t code)
{
    if (code == 0) {
        err(t, "null-character-reference");
        return 0xFFFD;
    }
    if (code > 0x10FFFF) {
        err(t, "character-reference-outside-unicode-range");
        return 0xFFFD;
    }
    if (code >= 0xD800 && code <= 0xDFFF) {
        err(t, "surrogate-character-reference");
        return 0xFFFD;
    }
    if (is_noncharacter(code)) {
        err(t, "noncharacter-character-reference");
    }
    if ((code < 0x20 && !is_ws(code)) || (code >= 0x7F && code <= 0x9F)) {
        err(t, "control-character-reference");
    }
    return numeric_replacement(code);
}

/* Consumes the characters of a named reference that matched. Fills the temporary buffer. */
static void finish_named(embi_html_tokenizer *t, size_t idx, uint32_t len)
{
    const ht_entity *e = &ht_entities[idx];
    adv_n(t, len);
    if (ht_ent_name(idx)[len - 1] != ';') {
        err(t, "missing-semicolon-after-character-reference");
    }
    t->ref_n = 0;
    ref_push(t, e->cp1);
    if (e->cp2i != 0) {
        ref_push(t, ht_entity_cp2[e->cp2i]);
    }
    flush_ref(t);
    t->state = t->ret;
}

/* ------------------------------------------------------------------ states */

static bool step(embi_html_tokenizer *t)
{
    uint32_t c;
    /* Each state that makes progress continues the loop, so one call runs as many states as the
     * input allows. Returns false when more input is needed, and true once the tokenizer has
     * paused or finished. */
    for (;;) {
        if (t->paused || t->done) {
            return true;
        }
        if (t->text.len >= HT_TEXT_FLUSH) {
            flush_text(t);
        }
        switch (t->state) {
        case HS_DATA: {
            PEEK(c);
            if (c == '&') {
                if (fast_ref(t, HS_DATA)) {
                    (void)data_run(t);
                    continue;
                }
                adv(t);
                enter_ref(t, HS_DATA);
                continue;
            }
            if (c == '<') {
                if (fast_tag(t)) {
                    continue;
                }
                adv(t);
                t->state = HS_TAG_OPEN;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0);
                continue;
            }
            if (c == HT_EOF) {
                emit_eof(t);
                continue;
            }
            /* Fast path: a run of plain text. */
            if (data_run(t) == 0) {
                adv(t);
                emit_cp(t, c);
            }
            continue;
        }

        case HS_RCDATA:
            PEEK(c);
            if (c == '&') {
                if (fast_ref(t, HS_RCDATA)) {
                    continue;
                }
                adv(t);
                enter_ref(t, HS_RCDATA);
                continue;
            }
            if (c == '<') {
                adv(t);
                t->state = HS_RCDATA_LT;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            continue;

        case HS_RAWTEXT:
            PEEK(c);
            if (c == '<') {
                adv(t);
                t->state = HS_RAWTEXT_LT;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            continue;

        case HS_SCRIPT_DATA:
            PEEK(c);
            if (c == '<') {
                adv(t);
                t->state = HS_SCRIPT_LT;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            continue;

        case HS_PLAINTEXT:
            PEEK(c);
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            continue;

        case HS_TAG_OPEN:
            PEEK(c);
            if (c == '!') {
                adv(t);
                t->state = HS_MARKUP_DECL_OPEN;
                continue;
            }
            if (c == '/') {
                adv(t);
                t->state = HS_END_TAG_OPEN;
                continue;
            }
            if (is_alpha(c)) {
                create_tag(t, false);
                t->state = HS_TAG_NAME;
                continue;
            }
            if (c == '?') {
                adv(t);
                sb_reset(&t->pi_target);
                comment_clear(t);
                t->state = HS_PI_OPEN;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-before-tag-name");
                emit_cp(t, '<');
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            err(t, "invalid-first-character-of-tag-name");
            emit_cp(t, '<');
            t->state = HS_DATA;
            continue;

        case HS_END_TAG_OPEN:
            PEEK(c);
            if (is_alpha(c)) {
                create_tag(t, true);
                t->state = HS_TAG_NAME;
                continue;
            }
            if (c == '>') {
                err(t, "missing-end-tag-name");
                adv(t);
                t->state = HS_DATA;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-before-tag-name");
                emit_lit(t, "</");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            err(t, "invalid-first-character-of-tag-name");
            comment_clear(t);
            t->state = HS_BOGUS_COMMENT;
            continue;

        case HS_TAG_NAME:
            if (bulk_run(t, HB_TAG, &t->tag_name, true) > 0) {
                continue;
            }
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_BEFORE_ATTR_NAME;
                continue;
            }
            if (c == '/') {
                adv(t);
                t->state = HS_SELF_CLOSING_START;
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_tag(t, 1);
                continue;
            }
            if (c == HT_EOF) {
                eof_in_tag(t);
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                sb_cp(&t->tag_name, 0xFFFD);
                continue;
            }
            adv(t);
            sb_cp(&t->tag_name, to_lower(c));
            continue;

        case HS_RCDATA_LT:
            return lt_state(t, HS_RCDATA_END_OPEN, HS_RCDATA);

        case HS_RCDATA_END_OPEN:
            return end_open_state(t, HS_RCDATA_END_NAME, HS_RCDATA);

        case HS_RCDATA_END_NAME:
            return end_name_state(t, HS_RCDATA);

        case HS_RAWTEXT_LT:
            return lt_state(t, HS_RAWTEXT_END_OPEN, HS_RAWTEXT);

        case HS_RAWTEXT_END_OPEN:
            return end_open_state(t, HS_RAWTEXT_END_NAME, HS_RAWTEXT);

        case HS_RAWTEXT_END_NAME:
            return end_name_state(t, HS_RAWTEXT);

        case HS_SCRIPT_LT:
            PEEK(c);
            if (c == '/') {
                adv(t);
                sb_reset(&t->temp);
                t->state = HS_SCRIPT_END_OPEN;
                continue;
            }
            if (c == '!') {
                adv(t);
                emit_lit(t, "<!");
                t->state = HS_SCRIPT_ESC_START;
                continue;
            }
            emit_cp(t, '<');
            t->state = HS_SCRIPT_DATA;
            continue;

        case HS_SCRIPT_END_OPEN:
            return end_open_state(t, HS_SCRIPT_END_NAME, HS_SCRIPT_DATA);

        case HS_SCRIPT_END_NAME:
            return end_name_state(t, HS_SCRIPT_DATA);

        case HS_SCRIPT_ESC_START:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                t->state = HS_SCRIPT_ESC_START_DASH;
                continue;
            }
            t->state = HS_SCRIPT_DATA;
            continue;

        case HS_SCRIPT_ESC_START_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                t->state = HS_SCRIPT_ESCAPED_DASH_DASH;
                continue;
            }
            t->state = HS_SCRIPT_DATA;
            continue;

        case HS_SCRIPT_ESCAPED:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                t->state = HS_SCRIPT_ESCAPED_DASH;
                continue;
            }
            if (c == '<') {
                adv(t);
                t->state = HS_SCRIPT_ESCAPED_LT;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            continue;

        case HS_SCRIPT_ESCAPED_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                t->state = HS_SCRIPT_ESCAPED_DASH_DASH;
                continue;
            }
            if (c == '<') {
                adv(t);
                t->state = HS_SCRIPT_ESCAPED_LT;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                t->state = HS_SCRIPT_ESCAPED;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            t->state = HS_SCRIPT_ESCAPED;
            continue;

        case HS_SCRIPT_ESCAPED_DASH_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                continue;
            }
            if (c == '<') {
                adv(t);
                t->state = HS_SCRIPT_ESCAPED_LT;
                continue;
            }
            if (c == '>') {
                adv(t);
                emit_cp(t, '>');
                t->state = HS_SCRIPT_DATA;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                t->state = HS_SCRIPT_ESCAPED;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            t->state = HS_SCRIPT_ESCAPED;
            continue;

        case HS_SCRIPT_ESCAPED_LT:
            PEEK(c);
            if (c == '/') {
                adv(t);
                sb_reset(&t->temp);
                t->state = HS_SCRIPT_ESCAPED_END_OPEN;
                continue;
            }
            if (is_alpha(c)) {
                sb_reset(&t->temp);
                emit_cp(t, '<');
                t->state = HS_SCRIPT_DBL_START;
                continue;
            }
            emit_cp(t, '<');
            t->state = HS_SCRIPT_ESCAPED;
            continue;

        case HS_SCRIPT_ESCAPED_END_OPEN:
            return end_open_state(t, HS_SCRIPT_ESCAPED_END_NAME, HS_SCRIPT_ESCAPED);

        case HS_SCRIPT_ESCAPED_END_NAME:
            return end_name_state(t, HS_SCRIPT_ESCAPED);

        case HS_SCRIPT_DBL_START:
            PEEK(c);
            if (is_ws(c) || c == '/' || c == '>') {
                adv(t);
                t->state = sb_is_script(&t->temp) ? HS_SCRIPT_DBL_ESCAPED : HS_SCRIPT_ESCAPED;
                emit_cp(t, c);
                continue;
            }
            if (is_alpha(c)) {
                adv(t);
                sb_cp(&t->temp, to_lower(c));
                emit_cp(t, c);
                continue;
            }
            t->state = HS_SCRIPT_ESCAPED;
            continue;

        case HS_SCRIPT_DBL_ESCAPED:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                t->state = HS_SCRIPT_DBL_ESCAPED_DASH;
                continue;
            }
            if (c == '<') {
                adv(t);
                emit_cp(t, '<');
                t->state = HS_SCRIPT_DBL_ESCAPED_LT;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            continue;

        case HS_SCRIPT_DBL_ESCAPED_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                t->state = HS_SCRIPT_DBL_ESCAPED_DASH_DASH;
                continue;
            }
            if (c == '<') {
                adv(t);
                emit_cp(t, '<');
                t->state = HS_SCRIPT_DBL_ESCAPED_LT;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                t->state = HS_SCRIPT_DBL_ESCAPED;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            t->state = HS_SCRIPT_DBL_ESCAPED;
            continue;

        case HS_SCRIPT_DBL_ESCAPED_DASH_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                emit_cp(t, '-');
                continue;
            }
            if (c == '<') {
                adv(t);
                emit_cp(t, '<');
                t->state = HS_SCRIPT_DBL_ESCAPED_LT;
                continue;
            }
            if (c == '>') {
                adv(t);
                emit_cp(t, '>');
                t->state = HS_SCRIPT_DATA;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                emit_cp(t, 0xFFFD);
                t->state = HS_SCRIPT_DBL_ESCAPED;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-script-html-comment-like-text");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            t->state = HS_SCRIPT_DBL_ESCAPED;
            continue;

        case HS_SCRIPT_DBL_ESCAPED_LT:
            PEEK(c);
            if (c == '/') {
                adv(t);
                sb_reset(&t->temp);
                emit_cp(t, '/');
                t->state = HS_SCRIPT_DBL_END;
                continue;
            }
            t->state = HS_SCRIPT_DBL_ESCAPED;
            continue;

        case HS_SCRIPT_DBL_END:
            PEEK(c);
            if (is_ws(c) || c == '/' || c == '>') {
                adv(t);
                t->state = sb_is_script(&t->temp) ? HS_SCRIPT_ESCAPED : HS_SCRIPT_DBL_ESCAPED;
                emit_cp(t, c);
                continue;
            }
            if (is_alpha(c)) {
                adv(t);
                sb_cp(&t->temp, to_lower(c));
                emit_cp(t, c);
                continue;
            }
            t->state = HS_SCRIPT_DBL_ESCAPED;
            continue;

        case HS_BEFORE_ATTR_NAME:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '/' || c == '>' || c == HT_EOF) {
                t->state = HS_AFTER_ATTR_NAME;
                continue;
            }
            if (c == '=') {
                err(t, "unexpected-equals-sign-before-attribute-name");
                adv(t);
                start_attr(t);
                attr_name_put(t, c);
                t->state = HS_ATTR_NAME;
                continue;
            }
            start_attr(t);
            t->state = HS_ATTR_NAME;
            continue;

        case HS_ATTR_NAME:
            if (bulk_run(t, HB_ATTR, &cur_attr(t)->name, true) > 0) {
                continue;
            }
            PEEK(c);
            if (is_ws(c) || c == '/' || c == '>' || c == HT_EOF) {
                check_dup(t);
                t->state = HS_AFTER_ATTR_NAME;
                continue;
            }
            if (c == '=') {
                check_dup(t);
                adv(t);
                t->state = HS_BEFORE_ATTR_VALUE;
                continue;
            }
            if (is_upper(c)) {
                adv(t);
                attr_name_put(t, to_lower(c));
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                attr_name_put(t, 0xFFFD);
                continue;
            }
            if (c == '"' || c == '\'' || c == '<') {
                err(t, "unexpected-character-in-attribute-name");
            }
            adv(t);
            attr_name_put(t, c);
            continue;

        case HS_AFTER_ATTR_NAME:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '/') {
                adv(t);
                t->state = HS_SELF_CLOSING_START;
                continue;
            }
            if (c == '=') {
                adv(t);
                t->state = HS_BEFORE_ATTR_VALUE;
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_tag(t, 1);
                continue;
            }
            if (c == HT_EOF) {
                eof_in_tag(t);
                continue;
            }
            start_attr(t);
            t->state = HS_ATTR_NAME;
            continue;

        case HS_BEFORE_ATTR_VALUE:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '"') {
                adv(t);
                t->state = HS_ATTR_VALUE_DQ;
                continue;
            }
            if (c == '\'') {
                adv(t);
                t->state = HS_ATTR_VALUE_SQ;
                continue;
            }
            if (c == '>') {
                err(t, "missing-attribute-value");
                adv(t);
                t->state = HS_DATA;
                emit_tag(t, 1);
                continue;
            }
            t->state = HS_ATTR_VALUE_UQ;
            continue;

        case HS_ATTR_VALUE_DQ:
        case HS_ATTR_VALUE_SQ: {
            uint32_t quote = t->state == HS_ATTR_VALUE_DQ ? '"' : '\'';
            if (bulk_run(t, quote == '"' ? HB_DQ : HB_SQ, &cur_attr(t)->value, false) > 0) {
                continue;
            }
            PEEK(c);
            if (c == quote) {
                adv(t);
                t->state = HS_AFTER_ATTR_VALUE_QUOTED;
                continue;
            }
            if (c == '&') {
                if (fast_ref(t, t->state)) {
                    continue;
                }
                adv(t);
                enter_ref(t, t->state);
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                attr_val_put(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                eof_in_tag(t);
                continue;
            }
            adv(t);
            attr_val_put(t, c);
            continue;
        }

        case HS_ATTR_VALUE_UQ:
            if (bulk_run(t, HB_UQ, &cur_attr(t)->value, false) > 0) {
                continue;
            }
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_BEFORE_ATTR_NAME;
                continue;
            }
            if (c == '&') {
                if (fast_ref(t, HS_ATTR_VALUE_UQ)) {
                    continue;
                }
                adv(t);
                enter_ref(t, HS_ATTR_VALUE_UQ);
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_tag(t, 1);
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                attr_val_put(t, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                eof_in_tag(t);
                continue;
            }
            if (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`') {
                err(t, "unexpected-character-in-unquoted-attribute-value");
            }
            adv(t);
            attr_val_put(t, c);
            continue;

        case HS_AFTER_ATTR_VALUE_QUOTED:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_BEFORE_ATTR_NAME;
                continue;
            }
            if (c == '/') {
                adv(t);
                t->state = HS_SELF_CLOSING_START;
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_tag(t, 1);
                continue;
            }
            if (c == HT_EOF) {
                eof_in_tag(t);
                continue;
            }
            err(t, "missing-whitespace-between-attributes");
            t->state = HS_BEFORE_ATTR_NAME;
            continue;

        case HS_SELF_CLOSING_START:
            PEEK(c);
            if (c == '>') {
                adv(t);
                t->self_closing = true;
                t->state = HS_DATA;
                emit_tag(t, 1);
                continue;
            }
            if (c == HT_EOF) {
                eof_in_tag(t);
                continue;
            }
            err(t, "unexpected-solidus-in-tag");
            t->state = HS_BEFORE_ATTR_NAME;
            continue;

        case HS_BOGUS_COMMENT:
            PEEK(c);
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_comment(t);
                continue;
            }
            if (c == HT_EOF) {
                t->state = HS_DATA;
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                sb_cp(&t->comment, 0xFFFD);
                continue;
            }
            adv(t);
            sb_cp(&t->comment, c);
            continue;

        case HS_PI_OPEN:
            PEEK(c);
            if (is_alpha(c) || c == '_') {
                t->state = HS_PI_TARGET;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-processing-instruction");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            err(t, "invalid-first-character-of-processing-instruction-target");
            comment_clear(t);
            sb_cp(&t->comment, '?');
            t->state = HS_BOGUS_COMMENT;
            continue;

        case HS_PI_TARGET:
            PEEK(c);
            if (is_alpha(c) || is_digit(c) || c == '-' || c == '_') {
                adv(t);
                sb_cp(&t->pi_target, c);
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-processing-instruction");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            if (is_ws(c) || c == '?' || c == '>') {
                pi_mark(t);
                embi_str tg = sb_str(&t->pi_target);
                if (pi_target_is_xml(tg)) {
                    /* xml and xml-stylesheet targets stay bogus comments. */
                    comment_clear(t);
                    sb_cp(&t->comment, '?');
                    embi_sb_append(&t->comment, tg);
                    t->state = HS_BOGUS_COMMENT;
                    continue;
                }
                adv(t);
                if (c == '>') {
                    t->state = HS_DATA;
                    emit_pi(t);
                } else if (c == '?') {
                    t->state = HS_PI_QUESTION;
                } else {
                    t->state = HS_PI_BEFORE_DATA;
                }
                continue;
            }
            /* Any other character: the target is invalid, so this is a bogus comment. */
            comment_clear(t);
            sb_cp(&t->comment, '?');
            embi_sb_append(&t->comment, sb_str(&t->pi_target));
            t->state = HS_BOGUS_COMMENT;
            continue;

        case HS_PI_BEFORE_DATA:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            t->state = HS_PI_DATA;
            continue;

        case HS_PI_DATA:
            PEEK(c);
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_pi(t);
                continue;
            }
            if (c == '?') {
                adv(t);
                t->state = HS_PI_QUESTION;
                continue;
            }
            if (c == HT_EOF) {
                pi_eof(t);
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                sb_cp(&t->comment, 0xFFFD);
                continue;
            }
            adv(t);
            sb_cp(&t->comment, c);
            continue;

        case HS_PI_QUESTION:
            PEEK(c);
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_pi(t);
                continue;
            }
            if (c == '?') {
                adv(t);
                sb_cp(&t->comment, '?');
                continue;
            }
            if (c == HT_EOF) {
                pi_eof(t);
                continue;
            }
            sb_cp(&t->comment, '?');
            t->state = HS_PI_DATA;
            continue;

        case HS_MARKUP_DECL_OPEN: {
            int r = ahead_lit(t, "--", false);
            if (r < 0) {
                return false;
            }
            if (r == 1) {
                adv_n(t, 2);
                comment_clear(t);
                t->state = HS_COMMENT_START;
                continue;
            }
            r = ahead_lit(t, "DOCTYPE", true);
            if (r < 0) {
                return false;
            }
            if (r == 1) {
                adv_n(t, 7);
                doctype_reset(t);
                t->state = HS_DOCTYPE;
                continue;
            }
            r = ahead_lit(t, "[CDATA[", false);
            if (r < 0) {
                return false;
            }
            if (r == 1 && t->cdata_allowed) {
                adv_n(t, 7);
                t->state = HS_CDATA;
                continue;
            }
            if (r == 1) {
                /* The spec makes "[CDATA[" the start of the comment data; the error is at its last char. */
                adv_n(t, 7);
                err_back(t, "cdata-in-html-content", 1);
                comment_clear(t);
                embi_sb_append(&t->comment, EMBI_STR("[CDATA["));
                t->state = HS_BOGUS_COMMENT;
                continue;
            }
            err(t, "incorrectly-opened-comment");
            comment_clear(t);
            t->state = HS_BOGUS_COMMENT;
            continue;
        }

        case HS_COMMENT_START:
            PEEK(c);
            if (c == '-') {
                adv(t);
                t->state = HS_COMMENT_START_DASH;
                continue;
            }
            if (c == '>') {
                err(t, "abrupt-closing-of-empty-comment");
                adv(t);
                t->state = HS_DATA;
                emit_comment(t);
                continue;
            }
            t->state = HS_COMMENT;
            continue;

        case HS_COMMENT_START_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                t->state = HS_COMMENT_END;
                continue;
            }
            if (c == '>') {
                err(t, "abrupt-closing-of-empty-comment");
                adv(t);
                t->state = HS_DATA;
                emit_comment(t);
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-comment");
                t->state = HS_DATA;
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            sb_cp(&t->comment, '-');
            t->state = HS_COMMENT;
            continue;

        case HS_COMMENT:
            PEEK(c);
            if (c == '<') {
                adv(t);
                sb_cp(&t->comment, '<');
                t->state = HS_COMMENT_LT;
                continue;
            }
            if (c == '-') {
                adv(t);
                t->state = HS_COMMENT_END_DASH;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                sb_cp(&t->comment, 0xFFFD);
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-comment");
                t->state = HS_DATA;
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            adv(t);
            sb_cp(&t->comment, c);
            continue;

        case HS_COMMENT_LT:
            PEEK(c);
            if (c == '!') {
                adv(t);
                sb_cp(&t->comment, '!');
                t->state = HS_COMMENT_LT_BANG;
                continue;
            }
            if (c == '<') {
                adv(t);
                sb_cp(&t->comment, '<');
                continue;
            }
            t->state = HS_COMMENT;
            continue;

        case HS_COMMENT_LT_BANG:
            PEEK(c);
            if (c == '-') {
                adv(t);
                t->state = HS_COMMENT_LT_BANG_DASH;
                continue;
            }
            t->state = HS_COMMENT;
            continue;

        case HS_COMMENT_LT_BANG_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                t->state = HS_COMMENT_LT_BANG_DASH_DASH;
                continue;
            }
            t->state = HS_COMMENT_END_DASH;
            continue;

        case HS_COMMENT_LT_BANG_DASH_DASH:
            PEEK(c);
            if (c != '>' && c != HT_EOF) {
                err(t, "nested-comment");
            }
            t->state = HS_COMMENT_END;
            continue;

        case HS_COMMENT_END_DASH:
            PEEK(c);
            if (c == '-') {
                adv(t);
                t->state = HS_COMMENT_END;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-comment");
                t->state = HS_DATA;
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            sb_cp(&t->comment, '-');
            t->state = HS_COMMENT;
            continue;

        case HS_COMMENT_END:
            PEEK(c);
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_comment(t);
                continue;
            }
            if (c == '!') {
                adv(t);
                t->state = HS_COMMENT_END_BANG;
                continue;
            }
            if (c == '-') {
                adv(t);
                sb_cp(&t->comment, '-');
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-comment");
                t->state = HS_DATA;
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            embi_sb_append(&t->comment, EMBI_STR("--"));
            t->state = HS_COMMENT;
            continue;

        case HS_COMMENT_END_BANG:
            PEEK(c);
            if (c == '-') {
                adv(t);
                embi_sb_append(&t->comment, EMBI_STR("--!"));
                t->state = HS_COMMENT_END_DASH;
                continue;
            }
            if (c == '>') {
                err(t, "incorrectly-closed-comment");
                adv(t);
                t->state = HS_DATA;
                emit_comment(t);
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-comment");
                t->state = HS_DATA;
                emit_comment(t);
                emit_eof(t);
                continue;
            }
            embi_sb_append(&t->comment, EMBI_STR("--!"));
            t->state = HS_COMMENT;
            continue;

        case HS_DOCTYPE:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_BEFORE_DOCTYPE_NAME;
                continue;
            }
            if (c == '>') {
                t->state = HS_BEFORE_DOCTYPE_NAME;
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "missing-whitespace-before-doctype-name");
            t->state = HS_BEFORE_DOCTYPE_NAME;
            continue;

        case HS_BEFORE_DOCTYPE_NAME:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                t->dt_has_name = true;
                sb_cp(&t->dt_name, 0xFFFD);
                t->state = HS_DOCTYPE_NAME;
                continue;
            }
            if (c == '>') {
                err(t, "missing-doctype-name");
                t->dt_force_quirks = true;
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            adv(t);
            t->dt_has_name = true;
            sb_cp(&t->dt_name, to_lower(c));
            t->state = HS_DOCTYPE_NAME;
            continue;

        case HS_DOCTYPE_NAME:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_AFTER_DOCTYPE_NAME;
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                sb_cp(&t->dt_name, 0xFFFD);
                continue;
            }
            adv(t);
            sb_cp(&t->dt_name, to_lower(c));
            continue;

        case HS_AFTER_DOCTYPE_NAME: {
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            int r = ahead_lit(t, "PUBLIC", true);
            if (r < 0) {
                return false;
            }
            if (r == 1) {
                adv_n(t, 6);
                t->state = HS_AFTER_DOCTYPE_PUBLIC_KW;
                continue;
            }
            r = ahead_lit(t, "SYSTEM", true);
            if (r < 0) {
                return false;
            }
            if (r == 1) {
                adv_n(t, 6);
                t->state = HS_AFTER_DOCTYPE_SYSTEM_KW;
                continue;
            }
            err(t, "invalid-character-sequence-after-doctype-name");
            t->dt_force_quirks = true;
            t->state = HS_BOGUS_DOCTYPE;
            continue;
        }

        case HS_AFTER_DOCTYPE_PUBLIC_KW:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_BEFORE_DOCTYPE_PUBLIC;
                continue;
            }
            if (c == '"' || c == '\'') {
                err(t, "missing-whitespace-after-doctype-public-keyword");
                t->dt_has_public = true;
                sb_reset(&t->dt_public);
                adv(t);
                t->state = c == '"' ? HS_DOCTYPE_PUBLIC_DQ : HS_DOCTYPE_PUBLIC_SQ;
                continue;
            }
            if (c == '>') {
                err(t, "missing-doctype-public-identifier");
                t->dt_force_quirks = true;
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "missing-quote-before-doctype-public-identifier");
            t->dt_force_quirks = true;
            t->state = HS_BOGUS_DOCTYPE;
            continue;

        case HS_BEFORE_DOCTYPE_PUBLIC:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '"' || c == '\'') {
                t->dt_has_public = true;
                sb_reset(&t->dt_public);
                adv(t);
                t->state = c == '"' ? HS_DOCTYPE_PUBLIC_DQ : HS_DOCTYPE_PUBLIC_SQ;
                continue;
            }
            if (c == '>') {
                err(t, "missing-doctype-public-identifier");
                t->dt_force_quirks = true;
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "missing-quote-before-doctype-public-identifier");
            t->dt_force_quirks = true;
            t->state = HS_BOGUS_DOCTYPE;
            continue;

        case HS_DOCTYPE_PUBLIC_DQ:
        case HS_DOCTYPE_PUBLIC_SQ: {
            uint32_t quote = t->state == HS_DOCTYPE_PUBLIC_DQ ? '"' : '\'';
            PEEK(c);
            if (c == quote) {
                adv(t);
                t->state = HS_AFTER_DOCTYPE_PUBLIC;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                sb_cp(&t->dt_public, 0xFFFD);
                continue;
            }
            if (c == '>') {
                err(t, "abrupt-doctype-public-identifier");
                t->dt_force_quirks = true;
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            adv(t);
            sb_cp(&t->dt_public, c);
            continue;
        }

        case HS_AFTER_DOCTYPE_PUBLIC:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_BETWEEN_DOCTYPE_IDS;
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == '"' || c == '\'') {
                err(t, "missing-whitespace-between-doctype-public-and-system-identifiers");
                t->dt_has_system = true;
                sb_reset(&t->dt_system);
                adv(t);
                t->state = c == '"' ? HS_DOCTYPE_SYSTEM_DQ : HS_DOCTYPE_SYSTEM_SQ;
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "missing-quote-before-doctype-system-identifier");
            t->dt_force_quirks = true;
            t->state = HS_BOGUS_DOCTYPE;
            continue;

        case HS_BETWEEN_DOCTYPE_IDS:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == '"' || c == '\'') {
                t->dt_has_system = true;
                sb_reset(&t->dt_system);
                adv(t);
                t->state = c == '"' ? HS_DOCTYPE_SYSTEM_DQ : HS_DOCTYPE_SYSTEM_SQ;
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "missing-quote-before-doctype-system-identifier");
            t->dt_force_quirks = true;
            t->state = HS_BOGUS_DOCTYPE;
            continue;

        case HS_AFTER_DOCTYPE_SYSTEM_KW:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                t->state = HS_BEFORE_DOCTYPE_SYSTEM;
                continue;
            }
            if (c == '"' || c == '\'') {
                err(t, "missing-whitespace-after-doctype-system-keyword");
                t->dt_has_system = true;
                sb_reset(&t->dt_system);
                adv(t);
                t->state = c == '"' ? HS_DOCTYPE_SYSTEM_DQ : HS_DOCTYPE_SYSTEM_SQ;
                continue;
            }
            if (c == '>') {
                err(t, "missing-doctype-system-identifier");
                t->dt_force_quirks = true;
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "missing-quote-before-doctype-system-identifier");
            t->dt_force_quirks = true;
            t->state = HS_BOGUS_DOCTYPE;
            continue;

        case HS_BEFORE_DOCTYPE_SYSTEM:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '"' || c == '\'') {
                t->dt_has_system = true;
                sb_reset(&t->dt_system);
                adv(t);
                t->state = c == '"' ? HS_DOCTYPE_SYSTEM_DQ : HS_DOCTYPE_SYSTEM_SQ;
                continue;
            }
            if (c == '>') {
                err(t, "missing-doctype-system-identifier");
                t->dt_force_quirks = true;
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "missing-quote-before-doctype-system-identifier");
            t->dt_force_quirks = true;
            t->state = HS_BOGUS_DOCTYPE;
            continue;

        case HS_DOCTYPE_SYSTEM_DQ:
        case HS_DOCTYPE_SYSTEM_SQ: {
            uint32_t quote = t->state == HS_DOCTYPE_SYSTEM_DQ ? '"' : '\'';
            PEEK(c);
            if (c == quote) {
                adv(t);
                t->state = HS_AFTER_DOCTYPE_SYSTEM;
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
                adv(t);
                sb_cp(&t->dt_system, 0xFFFD);
                continue;
            }
            if (c == '>') {
                err(t, "abrupt-doctype-system-identifier");
                t->dt_force_quirks = true;
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            adv(t);
            sb_cp(&t->dt_system, c);
            continue;
        }

        case HS_AFTER_DOCTYPE_SYSTEM:
            PEEK(c);
            if (is_ws(c)) {
                adv(t);
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                return doctype_eof(t);
            }
            err(t, "unexpected-character-after-doctype-system-identifier");
            t->state = HS_BOGUS_DOCTYPE;
            continue;

        case HS_BOGUS_DOCTYPE:
            PEEK(c);
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                emit_doctype(t);
                continue;
            }
            if (c == HT_EOF) {
                t->state = HS_DATA;
                emit_doctype(t);
                emit_eof(t);
                continue;
            }
            if (c == 0) {
                err(t, "unexpected-null-character");
            }
            adv(t);
            continue;

        case HS_CDATA:
            PEEK(c);
            if (c == ']') {
                adv(t);
                t->state = HS_CDATA_BRACKET;
                continue;
            }
            if (c == HT_EOF) {
                err(t, "eof-in-cdata");
                t->state = HS_DATA;
                emit_eof(t);
                continue;
            }
            adv(t);
            emit_cp(t, c);
            continue;

        case HS_CDATA_BRACKET:
            PEEK(c);
            if (c == ']') {
                adv(t);
                t->state = HS_CDATA_END;
                continue;
            }
            emit_cp(t, ']');
            t->state = HS_CDATA;
            continue;

        case HS_CDATA_END:
            PEEK(c);
            if (c == ']') {
                adv(t);
                emit_cp(t, ']');
                continue;
            }
            if (c == '>') {
                adv(t);
                t->state = HS_DATA;
                continue;
            }
            emit_lit(t, "]]");
            t->state = HS_CDATA;
            continue;

        case HS_CHAR_REF:
            PEEK(c);
            if (is_alnum(c)) {
                t->state = HS_NAMED_REF;
                continue;
            }
            if (c == '#') {
                adv(t);
                ref_push(t, '#');
                t->ref_code = 0;
                t->state = HS_NUMERIC_REF;
                continue;
            }
            flush_ref(t);
            t->state = t->ret;
            continue;

        case HS_NAMED_REF: {
            size_t idx = 0;
            uint32_t len = 0;
            int r = match_named(t, &idx, &len, is_attr_ret(t->ret));
            if (r < 0) {
                return false;
            }
            if (r == 0) {
                flush_ref(t);
                t->state = HS_AMBIGUOUS_AMP;
                continue;
            }
            if (r == 2) {
                /* Legacy name in an attribute followed by '=' or an alphanumeric: keep the text as written. */
                for (uint32_t i = 0; i < len; i++) {
                    ref_push(t, t->buf[t->pos + i]); /* a named reference is ASCII */
                }
                adv_n(t, len);
                flush_ref(t);
                t->state = t->ret;
                continue;
            }
            finish_named(t, idx, len);
            continue;
        }

        case HS_AMBIGUOUS_AMP:
            PEEK(c);
            if (is_alnum(c)) {
                adv(t);
                if (is_attr_ret(t->ret)) {
                    attr_val_put(t, c);
                } else {
                    emit_cp(t, c);
                }
                continue;
            }
            if (c == ';') {
                err(t, "unknown-named-character-reference");
            }
            t->state = t->ret;
            continue;

        case HS_NUMERIC_REF:
            PEEK(c);
            if (c == 'x' || c == 'X') {
                adv(t);
                ref_push(t, c);
                t->state = HS_HEX_START;
                continue;
            }
            t->state = HS_DEC_START;
            continue;

        case HS_HEX_START:
            PEEK(c);
            if (is_hex(c)) {
                t->state = HS_HEX_REF;
                continue;
            }
            err(t, "absence-of-digits-in-numeric-character-reference");
            flush_ref(t);
            t->state = t->ret;
            continue;

        case HS_DEC_START:
            PEEK(c);
            if (is_digit(c)) {
                t->state = HS_DEC_REF;
                continue;
            }
            err(t, "absence-of-digits-in-numeric-character-reference");
            flush_ref(t);
            t->state = t->ret;
            continue;

        case HS_HEX_REF:
        case HS_DEC_REF: {
            uint32_t base = t->state == HS_HEX_REF ? 16 : 10;
            PEEK(c);
            bool digit = base == 16 ? is_hex(c) : is_digit(c);
            if (digit) {
                adv(t);
                if (t->ref_code <= 0x10FFFF) {
                    t->ref_code = t->ref_code * base + hex_value(c);
                }
                continue;
            }
            if (c == ';') {
                adv(t);
                t->state = HS_NUMERIC_END;
                continue;
            }
            err(t, "missing-semicolon-after-character-reference");
            t->state = HS_NUMERIC_END;
            continue;
        }

        case HS_NUMERIC_END: {
            uint32_t code = numeric_code_checked(t, t->ref_code);
            t->ref_n = 0;
            ref_push(t, code);
            flush_ref(t);
            t->state = t->ret;
            continue;
        }

        default:
            EMBI_UNREACHABLE();
            return false;
        }
    }
}

/* Runs states until input runs out, the tokenizer pauses, or EOF has been emitted. */
static void run(embi_html_tokenizer *t)
{
    while (!t->paused && !t->done) {
        if (!step(t)) {
            break;
        }
        if (t->text.len >= HT_TEXT_FLUSH && !t->paused) {
            flush_text(t);
        }
    }
}

/* ------------------------------------------------------------------ input buffer */

/* Decides about a leading byte order mark, once the first bytes are here. Returns false while
 * more bytes are needed to tell whether one is present. A BOM is dropped when strip_bom is set
 * and is not counted in the stream positions. */
static bool start_check(embi_html_tokenizer *t, bool eof)
{
    if (!t->at_start) {
        return true;
    }
    size_t avail = t->len - t->pos;
    const uint8_t *p = t->buf + t->pos;
    bool bom = avail >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF;
    if (avail >= 3 || eof || !t->strip_bom) {
        if (t->strip_bom && bom) {
            t->pos += 3;
        }
        t->at_start = false;
        return true;
    }
    if ((avail >= 1 && p[0] != 0xEF) || (avail >= 2 && p[1] != 0xBB)) {
        t->at_start = false;
        return true;
    }
    return false;
}

/* Drops consumed bytes so the buffer does not grow with the stream. */
static void compact(embi_html_tokenizer *t)
{
    if (t->pos == 0) {
        return;
    }
    if (t->pos < t->len) {
        memmove(t->buf, t->buf + t->pos, t->len - t->pos);
    }
    t->len -= t->pos;
    t->pos = 0;
}

/* ------------------------------------------------------------------ public API */

embi_html_tokenizer *embi_html_tokenizer_create(embi_html_sink_fn sink,
                                                embi_html_error_fn on_error, void *ctx)
{
    EMBI_ASSERT(sink != nullptr);
    embi_html_tokenizer *t = embi_calloc(1, sizeof *t);
    t->sink = sink;
    t->on_error = on_error;
    t->ctx = ctx;
    t->state = HS_DATA;
    t->ret = HS_DATA;
    t->line = 1;
    t->at_start = true;
    /* The input buffer is never null, so pointer arithmetic on it is defined for empty input. */
    t->cap = 256;
    t->buf = embi_alloc(t->cap);
    return t;
}

void embi_html_tokenizer_destroy(embi_html_tokenizer *t)
{
    if (t == nullptr) {
        return;
    }
    embi_sb_free(&t->text);
    embi_sb_free(&t->temp);
    embi_sb_free(&t->comment);
    embi_sb_free(&t->pi_target);
    embi_sb_free(&t->tag_name);
    embi_sb_free(&t->last_start);
    embi_sb_free(&t->dt_name);
    embi_sb_free(&t->dt_public);
    embi_sb_free(&t->dt_system);
    for (size_t i = 0; i < t->attr_cap; i++) {
        embi_sb_free(&t->attrs[i].name);
        embi_sb_free(&t->attrs[i].value);
    }
    embi_free(t->attrs, t->attr_cap * sizeof *t->attrs);
    embi_free(t->out_attrs, t->out_cap * sizeof *t->out_attrs);
    embi_free(t->buf, t->cap);
    embi_free(t, sizeof *t);
}

void embi_html_tokenizer_set_state(embi_html_tokenizer *t, embi_html_state state)
{
    switch (state) {
    case EMBI_HTML_STATE_DATA: t->state = HS_DATA; break;
    case EMBI_HTML_STATE_RCDATA: t->state = HS_RCDATA; break;
    case EMBI_HTML_STATE_RAWTEXT: t->state = HS_RAWTEXT; break;
    case EMBI_HTML_STATE_SCRIPT_DATA: t->state = HS_SCRIPT_DATA; break;
    case EMBI_HTML_STATE_PLAINTEXT: t->state = HS_PLAINTEXT; break;
    case EMBI_HTML_STATE_CDATA_SECTION: t->state = HS_CDATA; break;
    default: EMBI_UNREACHABLE(); break;
    }
}

void embi_html_tokenizer_set_last_start_tag(embi_html_tokenizer *t, embi_str name)
{
    sb_reset(&t->last_start);
    if (name.len > 0) {
        embi_sb_append(&t->last_start, name);
    }
}

void embi_html_tokenizer_want_atoms(embi_html_tokenizer *t, bool on)
{
    t->want_atoms = on;
}

void embi_html_tokenizer_set_cdata_allowed(embi_html_tokenizer *t, bool allowed)
{
    t->cdata_allowed = allowed;
}

void embi_html_tokenizer_strip_bom(embi_html_tokenizer *t)
{
    t->strip_bom = true;
}

size_t embi_html_tokenizer_write(embi_html_tokenizer *t, const void *bytes, size_t n)
{
    const uint8_t *p = bytes;
    size_t accepted = 0;
    if (t->eof || t->done) {
        return 0;
    }
    while (accepted < n && !t->paused) {
        size_t take = n - accepted;
        if (take > HT_FEED_SLICE) {
            take = HT_FEED_SLICE;
        }
        compact(t);
        if (t->cap < t->len + take) {
            size_t cap = t->cap ? t->cap : 1024;
            while (cap < t->len + take) {
                cap *= 2;
            }
            t->buf = embi_realloc(t->buf, t->cap, cap);
            t->cap = cap;
        }
        /* The bytes are kept as they are: the state machine decodes them as it peeks. A slice may
         * end inside a character; the decoder then waits for the next slice. */
        memcpy(t->buf + t->len, p + accepted, take);
        t->len += take;
        accepted += take;
        if (start_check(t, false)) {
            run(t);
        }
    }
    return accepted;
}

void embi_html_tokenizer_flush_text(embi_html_tokenizer *t)
{
    if (!t->paused) {
        flush_text(t);
    }
}

void embi_html_tokenizer_resume(embi_html_tokenizer *t)
{
    t->paused = false;
    run(t);
}

bool embi_html_tokenizer_paused(const embi_html_tokenizer *t)
{
    return t->paused;
}

void embi_html_tokenizer_end(embi_html_tokenizer *t)
{
    if (!t->eof) {
        t->eof = true;
    }
    start_check(t, true);
    run(t);
}
