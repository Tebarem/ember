/*
 * CSS Syntax Level 3, section 3.3 (input preprocessing) and section 4 (tokenization).
 * Output goes into an arena. The whole input is tokenized up front.
 */
#include "css/css.h"
#include "num/num.h"

#include <string.h>

#define CSS_REPL 0xFFFDu

typedef struct css_tk {
    const char *s;
    uint32_t n;
    uint32_t i;
    uint32_t tok_start;
    embi_arena *a;
    embi_sb sb;
    embi_css_token_vec out;
} css_tk;

/* ------------------------------------------------------------------ preprocessing */

/* Writes the preprocessed text to out, or only counts it when out is nullptr. */
static size_t preprocess(embi_str in, char *out)
{
    const unsigned char *s = (const unsigned char *)in.ptr;
    size_t n = in.len;
    size_t o = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        uint32_t cp;
        size_t adv;
        if (c == '\r') {
            adv = (i + 1 < n && s[i + 1] == '\n') ? 2 : 1;
            cp = '\n';
        } else if (c == '\f') {
            adv = 1;
            cp = '\n';
        } else if (c == 0) {
            adv = 1;
            cp = CSS_REPL;
        } else if (c < 0x80) {
            adv = 1;
            cp = c;
        } else if (c == 0xED && i + 2 < n && (s[i + 1] & 0xE0) == 0xA0 &&
                   (s[i + 2] & 0xC0) == 0x80) {
            /* Encoded surrogate: one replacement character for the three bytes. */
            adv = 3;
            cp = CSS_REPL;
        } else {
            const char *p = in.ptr + i;
            cp = embi_utf8_decode(&p, in.ptr + n);
            adv = (size_t)(p - (in.ptr + i));
            if (adv == 0) {
                adv = 1;
            }
        }
        i += adv;
        char tmp[4];
        size_t k = embi_utf8_encode(cp, tmp);
        if (out) {
            memcpy(out + o, tmp, k);
        }
        o += k;
    }
    return o;
}

/* ------------------------------------------------------------------ character helpers */

static int at(const css_tk *t, uint32_t i)
{
    return i < t->n ? (unsigned char)t->s[i] : -1;
}

static bool dig(int c)
{
    return c >= '0' && c <= '9';
}

static bool hexd(int c)
{
    return dig(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hexv(int c)
{
    return dig(c) ? (uint32_t)(c - '0') : (uint32_t)((c | 0x20) - 'a' + 10);
}

static bool wsc(int c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static bool nonprintable(int c)
{
    return (c >= 0 && c <= 8) || c == 0x0B || (c >= 0x0E && c <= 0x1F) || c == 0x7F;
}

static bool ascii_name_byte(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || dig(c) || c == '_' || c == '-';
}

/* Decodes the code point at i (input is valid UTF-8 after preprocessing). */
static uint32_t cp_at(const css_tk *t, uint32_t i, uint32_t *len)
{
    unsigned char c = (unsigned char)t->s[i];
    if (c < 0x80) {
        *len = 1;
        return c;
    }
    const char *p = t->s + i;
    uint32_t cp = embi_utf8_decode(&p, t->s + t->n);
    *len = (uint32_t)(p - (t->s + i));
    if (*len == 0) {
        *len = 1;
    }
    return cp;
}

static bool name_start_at(const css_tk *t, uint32_t i)
{
    int c = at(t, i);
    if (c < 0) {
        return false;
    }
    if (c < 0x80) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    return true;
}

static bool name_at(const css_tk *t, uint32_t i)
{
    int c = at(t, i);
    if (c < 0) {
        return false;
    }
    return c >= 0x80 || ascii_name_byte(c);
}

/* A valid escape starts with a backslash that is not followed by a newline. EOF after the
 * backslash counts as valid and decodes to U+FFFD. */
static bool valid_escape(const css_tk *t, uint32_t i)
{
    return at(t, i) == '\\' && at(t, i + 1) != '\n';
}

static bool starts_ident(const css_tk *t, uint32_t i)
{
    int c = at(t, i);
    if (c == '-') {
        return at(t, i + 1) == '-' || name_start_at(t, i + 1) || valid_escape(t, i + 1);
    }
    if (name_start_at(t, i)) {
        return true;
    }
    return c == '\\' && valid_escape(t, i);
}

static bool starts_number(const css_tk *t, uint32_t i)
{
    int c = at(t, i);
    if (c == '+' || c == '-') {
        return dig(at(t, i + 1)) || (at(t, i + 1) == '.' && dig(at(t, i + 2)));
    }
    if (c == '.') {
        return dig(at(t, i + 1));
    }
    return dig(c);
}

/* ------------------------------------------------------------------ emitting tokens */

static void emit(css_tk *t, uint8_t type)
{
    embi_css_token tok;
    memset(&tok, 0, sizeof tok);
    tok.type = type;
    tok.start = t->tok_start;
    tok.end = t->i;
    embi_vec_push(&t->out, tok);
}

static embi_css_token *last_token(css_tk *t)
{
    return &t->out.data[t->out.len - 1];
}

static embi_str arena_str(css_tk *t, const char *p, size_t len)
{
    char *d = embi_arena_alloc(t->a, len + 1, 1);
    if (len) {
        memcpy(d, p, len);
    }
    d[len] = '\0';
    return (embi_str){d, (uint32_t)len};
}

static embi_str sb_to_arena(css_tk *t)
{
    return arena_str(t, t->sb.data ? t->sb.data : "", t->sb.len);
}

/* Emits a token whose value is the scratch buffer. */
static void emit_value(css_tk *t, uint8_t type)
{
    emit(t, type);
    last_token(t)->value = sb_to_arena(t);
}

/* ------------------------------------------------------------------ escapes and names */

/* t->i is at the backslash of a valid escape. Returns the code point and advances past it. */
static uint32_t consume_escape(css_tk *t)
{
    t->i++;
    int c = at(t, t->i);
    if (c < 0) {
        return CSS_REPL;
    }
    if (hexd(c)) {
        uint32_t v = 0;
        int k = 0;
        while (k < 6 && hexd(at(t, t->i))) {
            v = v * 16 + hexv(at(t, t->i));
            t->i++;
            k++;
        }
        if (wsc(at(t, t->i))) {
            t->i++;
        }
        if (v == 0 || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) {
            return CSS_REPL;
        }
        return v;
    }
    uint32_t len;
    uint32_t cp = cp_at(t, t->i, &len);
    t->i += len;
    return cp;
}

/* Consumes a name. The result is a slice of the input when it has no escapes, otherwise a copy
 * in the arena. */
static embi_str consume_name(css_tk *t)
{
    uint32_t start = t->i;
    bool esc = false;
    while (t->i < t->n) {
        int c = at(t, t->i);
        if (c == '\\') {
            if (valid_escape(t, t->i)) {
                esc = true;
            }
            break;
        }
        if (c < 0x80) {
            if (!ascii_name_byte(c)) {
                break;
            }
            t->i++;
            continue;
        }
        uint32_t len;
        cp_at(t, t->i, &len);
        t->i += len;
    }
    if (!esc) {
        return (embi_str){t->s + start, t->i - start};
    }
    embi_sb_clear(&t->sb);
    embi_sb_append(&t->sb, (embi_str){t->s + start, t->i - start});
    while (t->i < t->n) {
        int c = at(t, t->i);
        if (c == '\\') {
            if (!valid_escape(t, t->i)) {
                break;
            }
            embi_sb_append_cp(&t->sb, consume_escape(t));
            continue;
        }
        if (c < 0x80) {
            if (!ascii_name_byte(c)) {
                break;
            }
            embi_sb_append_char(&t->sb, (char)c);
            t->i++;
            continue;
        }
        uint32_t len;
        cp_at(t, t->i, &len);
        embi_sb_append(&t->sb, (embi_str){t->s + t->i, len});
        t->i += len;
    }
    return sb_to_arena(t);
}

/* ------------------------------------------------------------------ numbers */

static void consume_numeric(css_tk *t)
{
    uint32_t start = t->i;
    bool integer = true;
    if (at(t, t->i) == '+' || at(t, t->i) == '-') {
        t->i++;
    }
    while (dig(at(t, t->i))) {
        t->i++;
    }
    if (at(t, t->i) == '.' && dig(at(t, t->i + 1))) {
        integer = false;
        t->i++;
        while (dig(at(t, t->i))) {
            t->i++;
        }
    }
    int e = at(t, t->i);
    if (e == 'e' || e == 'E') {
        int c1 = at(t, t->i + 1);
        if (dig(c1)) {
            integer = false;
            t->i += 1;
        } else if ((c1 == '+' || c1 == '-') && dig(at(t, t->i + 2))) {
            integer = false;
            t->i += 2;
        }
        if (!integer) {
            while (dig(at(t, t->i))) {
                t->i++;
            }
        }
    }
    embi_str repr = {t->s + start, t->i - start};
    size_t used = 0;
    double value = embi_num_parse_decimal(repr.ptr, repr.len, &used);

    uint8_t type;
    embi_str unit = {nullptr, 0};
    if (starts_ident(t, t->i)) {
        type = EMBI_CSS_DIMENSION;
        unit = consume_name(t);
        unit = arena_str(t, unit.ptr, unit.len);
    } else if (at(t, t->i) == '%') {
        t->i++;
        type = EMBI_CSS_PERCENTAGE;
    } else {
        type = EMBI_CSS_NUMBER;
    }
    emit(t, type);
    embi_css_token *tok = last_token(t);
    tok->flag = integer;
    tok->repr = repr;
    tok->num = value;
    if (type == EMBI_CSS_DIMENSION) {
        tok->value = unit;
    }
}

/* ------------------------------------------------------------------ strings and urls */

static void consume_string(css_tk *t, int quote)
{
    t->i++;
    embi_sb_clear(&t->sb);
    for (;;) {
        int c = at(t, t->i);
        if (c < 0) {
            emit_value(t, EMBI_CSS_STRING);
            last_token(t)->flag = true;
            return;
        }
        if (c == quote) {
            t->i++;
            emit_value(t, EMBI_CSS_STRING);
            return;
        }
        if (c == '\n') {
            emit(t, EMBI_CSS_BAD_STRING);
            return;
        }
        if (c == '\\') {
            if (t->i + 1 >= t->n) {
                t->i++;
                continue;
            }
            if (at(t, t->i + 1) == '\n') {
                t->i += 2;
                continue;
            }
            embi_sb_append_cp(&t->sb, consume_escape(t));
            continue;
        }
        uint32_t len;
        cp_at(t, t->i, &len);
        embi_sb_append(&t->sb, (embi_str){t->s + t->i, len});
        t->i += len;
    }
}

/* Skips the remnants of a bad url up to and including the closing parenthesis or EOF. */
static void bad_url_remnants(css_tk *t)
{
    for (;;) {
        int c = at(t, t->i);
        if (c < 0) {
            return;
        }
        if (c == ')') {
            t->i++;
            return;
        }
        if (c == '\\' && valid_escape(t, t->i)) {
            consume_escape(t);
            continue;
        }
        uint32_t len;
        cp_at(t, t->i, &len);
        t->i += len;
    }
}

/* t->i is just after "url(". */
static void consume_url(css_tk *t)
{
    while (wsc(at(t, t->i))) {
        t->i++;
    }
    embi_sb_clear(&t->sb);
    for (;;) {
        int c = at(t, t->i);
        if (c < 0) {
            emit_value(t, EMBI_CSS_URL);
            last_token(t)->flag = true;
            return;
        }
        if (c == ')') {
            t->i++;
            emit_value(t, EMBI_CSS_URL);
            return;
        }
        if (wsc(c)) {
            while (wsc(at(t, t->i))) {
                t->i++;
            }
            c = at(t, t->i);
            if (c == ')') {
                t->i++;
                emit_value(t, EMBI_CSS_URL);
                return;
            }
            if (c < 0) {
                emit_value(t, EMBI_CSS_URL);
                last_token(t)->flag = true;
                return;
            }
            bad_url_remnants(t);
            emit(t, EMBI_CSS_BAD_URL);
            return;
        }
        if (c == '"' || c == '\'' || c == '(' || nonprintable(c)) {
            bad_url_remnants(t);
            emit(t, EMBI_CSS_BAD_URL);
            return;
        }
        if (c == '\\') {
            if (!valid_escape(t, t->i)) {
                bad_url_remnants(t);
                emit(t, EMBI_CSS_BAD_URL);
                return;
            }
            embi_sb_append_cp(&t->sb, consume_escape(t));
            continue;
        }
        uint32_t len;
        cp_at(t, t->i, &len);
        embi_sb_append(&t->sb, (embi_str){t->s + t->i, len});
        t->i += len;
    }
}

/* ------------------------------------------------------------------ identifiers and friends */

static void consume_ident_like(css_tk *t)
{
    embi_str name = consume_name(t);
    if (embi_str_ieq(name, EMBI_STR("url")) && at(t, t->i) == '(') {
        uint32_t j = t->i + 1;
        while (wsc(at(t, j))) {
            j++;
        }
        int q = at(t, j);
        if (q == '"' || q == '\'') {
            t->i++;
            emit(t, EMBI_CSS_FUNCTION);
            last_token(t)->value = name;
            return;
        }
        t->i++;
        consume_url(t);
        return;
    }
    if (at(t, t->i) == '(') {
        t->i++;
        emit(t, EMBI_CSS_FUNCTION);
        last_token(t)->value = name;
        return;
    }
    emit(t, EMBI_CSS_IDENT);
    last_token(t)->value = name;
}

/* U+ unicode-range. t->i is at the 'u' or 'U'. */
static void consume_unicode_range(css_tk *t)
{
    t->i += 2;
    uint32_t lo = 0;
    uint32_t hi = 0;
    int digits = 0;
    while (digits < 6 && hexd(at(t, t->i))) {
        uint32_t v = hexv(at(t, t->i));
        lo = lo * 16 + v;
        hi = hi * 16 + v;
        t->i++;
        digits++;
    }
    if (digits < 6 && at(t, t->i) == '?') {
        while (digits < 6 && at(t, t->i) == '?') {
            lo *= 16;
            hi = hi * 16 + 15;
            t->i++;
            digits++;
        }
    } else if (at(t, t->i) == '-' && hexd(at(t, t->i + 1))) {
        t->i++;
        hi = 0;
        int c = 0;
        while (c < 6 && hexd(at(t, t->i))) {
            hi = hi * 16 + hexv(at(t, t->i));
            t->i++;
            c++;
        }
    } else {
        hi = lo;
    }
    emit(t, EMBI_CSS_UNICODE_RANGE);
    last_token(t)->cp = lo;
    last_token(t)->cp_end = hi;
}

/* ------------------------------------------------------------------ main loop */

static void skip_comment(css_tk *t)
{
    t->i += 2;
    while (t->i < t->n && !(at(t, t->i) == '*' && at(t, t->i + 1) == '/')) {
        t->i++;
    }
    if (at(t, t->i) == '*') {
        t->i += 2;
    }
}

static void emit_delim(css_tk *t)
{
    uint32_t len;
    uint32_t cp = cp_at(t, t->i, &len);
    t->i += len;
    emit(t, EMBI_CSS_DELIM);
    last_token(t)->cp = cp;
}

static void emit_single(css_tk *t, uint8_t type)
{
    t->i++;
    emit(t, type);
}

static void tokenize(css_tk *t)
{
    while (t->i < t->n) {
        t->tok_start = t->i;
        int c = at(t, t->i);
        if (c == '/' && at(t, t->i + 1) == '*') {
            skip_comment(t);
            continue;
        }
        if (wsc(c)) {
            while (wsc(at(t, t->i))) {
                t->i++;
            }
            emit(t, EMBI_CSS_WS);
            continue;
        }
        switch (c) {
        case '"':
        case '\'':
            consume_string(t, c);
            continue;
        case '#':
            if (name_at(t, t->i + 1) || valid_escape(t, t->i + 1)) {
                t->i++;
                bool id = starts_ident(t, t->i);
                embi_str name = consume_name(t);
                emit(t, EMBI_CSS_HASH);
                last_token(t)->value = name;
                last_token(t)->flag = id;
            } else {
                emit_delim(t);
            }
            continue;
        case '(':
            emit_single(t, EMBI_CSS_LPAREN);
            continue;
        case ')':
            emit_single(t, EMBI_CSS_RPAREN);
            continue;
        case '[':
            emit_single(t, EMBI_CSS_LBRACKET);
            continue;
        case ']':
            emit_single(t, EMBI_CSS_RBRACKET);
            continue;
        case '{':
            emit_single(t, EMBI_CSS_LBRACE);
            continue;
        case '}':
            emit_single(t, EMBI_CSS_RBRACE);
            continue;
        case ',':
            emit_single(t, EMBI_CSS_COMMA);
            continue;
        case ':':
            emit_single(t, EMBI_CSS_COLON);
            continue;
        case ';':
            emit_single(t, EMBI_CSS_SEMICOLON);
            continue;
        case '+':
        case '.':
            if (starts_number(t, t->i)) {
                consume_numeric(t);
            } else {
                emit_delim(t);
            }
            continue;
        case '-':
            if (starts_number(t, t->i)) {
                consume_numeric(t);
            } else if (at(t, t->i + 1) == '-' && at(t, t->i + 2) == '>') {
                t->i += 3;
                emit(t, EMBI_CSS_CDC);
            } else if (starts_ident(t, t->i)) {
                consume_ident_like(t);
            } else {
                emit_delim(t);
            }
            continue;
        case '<':
            if (at(t, t->i + 1) == '!' && at(t, t->i + 2) == '-' && at(t, t->i + 3) == '-') {
                t->i += 4;
                emit(t, EMBI_CSS_CDO);
            } else {
                emit_delim(t);
            }
            continue;
        case '@':
            if (starts_ident(t, t->i + 1)) {
                t->i++;
                embi_str name = consume_name(t);
                emit(t, EMBI_CSS_AT_KEYWORD);
                last_token(t)->value = name;
            } else {
                emit_delim(t);
            }
            continue;
        case '\\':
            if (valid_escape(t, t->i)) {
                consume_ident_like(t);
            } else {
                emit_delim(t);
            }
            continue;
        case '~':
            if (at(t, t->i + 1) == '=') {
                t->i += 2;
                emit(t, EMBI_CSS_INCLUDE_MATCH);
            } else {
                emit_delim(t);
            }
            continue;
        case '|':
            if (at(t, t->i + 1) == '=') {
                t->i += 2;
                emit(t, EMBI_CSS_DASH_MATCH);
            } else if (at(t, t->i + 1) == '|') {
                t->i += 2;
                emit(t, EMBI_CSS_COLUMN);
            } else {
                emit_delim(t);
            }
            continue;
        case '^':
            if (at(t, t->i + 1) == '=') {
                t->i += 2;
                emit(t, EMBI_CSS_PREFIX_MATCH);
            } else {
                emit_delim(t);
            }
            continue;
        case '$':
            if (at(t, t->i + 1) == '=') {
                t->i += 2;
                emit(t, EMBI_CSS_SUFFIX_MATCH);
            } else {
                emit_delim(t);
            }
            continue;
        case '*':
            if (at(t, t->i + 1) == '=') {
                t->i += 2;
                emit(t, EMBI_CSS_SUBSTRING_MATCH);
            } else {
                emit_delim(t);
            }
            continue;
        default:
            break;
        }
        if (dig(c)) {
            consume_numeric(t);
            continue;
        }
        if ((c == 'u' || c == 'U') && at(t, t->i + 1) == '+' &&
            (hexd(at(t, t->i + 2)) || at(t, t->i + 2) == '?')) {
            consume_unicode_range(t);
            continue;
        }
        if (name_start_at(t, t->i)) {
            consume_ident_like(t);
            continue;
        }
        emit_delim(t);
    }
}

void embi_css_tokenize_vec(embi_arena *a, embi_str text, embi_css_token_vec *out)
{
    memset(out, 0, sizeof *out);
    /* The common case: ASCII with no CR, FF or NUL changes nothing. */
    bool plain = true;
    for (size_t i = 0; i < text.len; i++) {
        unsigned char c = (unsigned char)text.ptr[i];
        if (c >= 0x80 || c == '\r' || c == '\f' || c == 0) {
            plain = false;
            break;
        }
    }
    size_t n = plain ? text.len : preprocess(text, nullptr);
    if (n >= UINT32_MAX) {
        embi_log_error("css: input of %llu bytes after preprocessing is too large", (unsigned long long)n);
        return;
    }
    char *buf = embi_arena_alloc(a, n + 1, 1);
    if (plain) {
        if (n != 0) {
            memcpy(buf, text.ptr, n);
        }
    } else {
        preprocess(text, buf);
    }
    buf[n] = '\0';

    css_tk t;
    memset(&t, 0, sizeof t);
    t.s = buf;
    t.n = (uint32_t)n;
    t.a = a;
    /* About one token per 3 bytes in real style sheets: reserving avoids the regrowth copies (untouched pages stay
     * uncommitted). */
    if (n >= 4096) {
        embi_vec_reserve(&t.out, (uint32_t)(n / 3 + 16));
    }
    tokenize(&t);

    embi_sb_free(&t.sb);
    *out = t.out;
}

embi_css_tokens embi_css_tokenize(embi_arena *a, embi_str text)
{
    embi_css_token_vec v;
    embi_css_tokenize_vec(a, text, &v);
    embi_css_tokens r;
    r.n = v.len;
    r.p = embi_css_arena_copy(a, v.data, v.len, sizeof(embi_css_token));
    embi_vec_free(&v);
    return r;
}
