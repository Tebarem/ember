/*
 * WHATWG URL (https://url.spec.whatwg.org/): the basic URL parser with every state and the state
 * override used by the API setters, host parsing (domains through IDNA, IPv4, IPv6, opaque
 * hosts), the serializer, origin, and application/x-www-form-urlencoded.
 *
 * Internally a URL is a record of byte strings (urec). The public embi_url packs a record into
 * one string with offsets, and a setter unpacks, changes the record and packs again.
 *
 * The record's path is stored the way the spec's path list serializes: each segment is written as
 * "/" followed by the segment, so "" is the empty list, "/" is one empty segment, and "/a/b" is two
 * segments. An opaque path is stored raw. The buffer used by the states holds UTF-8.
 */
#include "loader/loader.h"

#include <string.h>

#define EOFC 0xFFFFFFFFu

/* States of the basic URL parser (spec 4.4). ST_NONE means no state override. */
enum url_state {
    ST_SCHEME_START,
    ST_SCHEME,
    ST_NO_SCHEME,
    ST_SPECIAL_RELATIVE_OR_AUTHORITY,
    ST_PATH_OR_AUTHORITY,
    ST_RELATIVE,
    ST_RELATIVE_SLASH,
    ST_SPECIAL_AUTHORITY_SLASHES,
    ST_SPECIAL_AUTHORITY_IGNORE_SLASHES,
    ST_AUTHORITY,
    ST_HOST,
    ST_HOSTNAME,
    ST_PORT,
    ST_FILE,
    ST_FILE_SLASH,
    ST_FILE_HOST,
    ST_PATH_START,
    ST_PATH,
    ST_OPAQUE_PATH,
    ST_QUERY,
    ST_FRAGMENT,
};

#define ST_NONE (-1)

/* Percent-encode sets (spec 1.3). */
enum pct_set {
    PS_C0,
    PS_FRAGMENT,
    PS_QUERY,
    PS_SPECIAL_QUERY,
    PS_PATH,
    PS_USERINFO,
};

typedef struct urec {
    embi_sb scheme;
    embi_sb username;
    embi_sb password;
    embi_sb host;
    bool has_host;
    int32_t port; /* -1 when null */
    embi_sb path;
    bool opaque;
    embi_sb query;
    bool has_query;
    embi_sb frag;
    bool has_frag;
} urec;

typedef EMBI_VEC(uint32_t) cpvec;

/* ------------------------------------------------------------------ small helpers */

static void sb_set(embi_sb *dst, const embi_sb *src)
{
    dst->len = 0;
    if (src->len > 0) {
        embi_sb_append(dst, (embi_str){src->data, src->len});
    }
}

static void sb_set_str(embi_sb *dst, embi_str s)
{
    dst->len = 0;
    if (s.len > 0) {
        embi_sb_append(dst, s);
    }
}

static bool sb_is(const embi_sb *s, const char *lit)
{
    size_t n = strlen(lit);
    return s->len == n && (n == 0 || memcmp(s->data, lit, n) == 0);
}

/* One past the last byte, without forming a pointer from a null base. */
static const char *sb_end(const embi_sb *s)
{
    return s->data != nullptr ? s->data + s->len : nullptr;
}

static bool sb_eq(const embi_sb *a, const embi_sb *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

static void urec_free(urec *u)
{
    embi_sb_free(&u->scheme);
    embi_sb_free(&u->username);
    embi_sb_free(&u->password);
    embi_sb_free(&u->host);
    embi_sb_free(&u->path);
    embi_sb_free(&u->query);
    embi_sb_free(&u->frag);
    *u = (urec){.port = -1};
}

static void urec_init(urec *u)
{
    *u = (urec){.port = -1};
}

static bool is_alpha(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(uint32_t c)
{
    return c >= '0' && c <= '9';
}

static bool is_alnum(uint32_t c)
{
    return is_alpha(c) || is_digit(c);
}

static bool is_hex(uint32_t c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex_value(uint32_t c)
{
    if (is_digit(c)) {
        return c - '0';
    }
    return (c | 0x20u) - 'a' + 10;
}

static uint32_t lower_cp(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static bool special_name(const char *s, size_t n)
{
    static const char *const names[] = {"ftp", "file", "http", "https", "ws", "wss"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t l = strlen(names[i]);
        if (l == n && memcmp(names[i], s, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool urec_special(const urec *u)
{
    return special_name((const char *)u->scheme.data, u->scheme.len);
}

static int32_t default_port_of(const embi_sb *scheme)
{
    if (sb_is(scheme, "ftp")) {
        return 21;
    }
    if (sb_is(scheme, "http") || sb_is(scheme, "ws")) {
        return 80;
    }
    if (sb_is(scheme, "https") || sb_is(scheme, "wss")) {
        return 443;
    }
    return -1;
}

static bool has_credentials(const urec *u)
{
    return u->username.len > 0 || u->password.len > 0;
}

/* Cannot have a username, password or port: no host, an empty host, or the file scheme. */
static bool cannot_have_userinfo(const urec *u)
{
    return !u->has_host || u->host.len == 0 || sb_is(&u->scheme, "file");
}

static bool in_pct_set(uint32_t c, enum pct_set set)
{
    if (c < 0x20u || c > 0x7Eu) {
        return true;
    }
    if (set == PS_C0) {
        return false;
    }
    if (set == PS_FRAGMENT) {
        return c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';
    }
    bool query = c == ' ' || c == '"' || c == '#' || c == '<' || c == '>';
    if (set == PS_QUERY) {
        return query;
    }
    if (set == PS_SPECIAL_QUERY) {
        return query || c == '\'';
    }
    bool path = query || c == '?' || c == '`' || c == '{' || c == '}';
    if (set == PS_PATH) {
        return path || c == '^';
    }
    return path || c == '/' || c == ':' || c == ';' || c == '=' || c == '@' ||
           (c >= '[' && c <= '^') || c == '|';
}

static void append_pct_byte(embi_sb *out, unsigned char b)
{
    static const char hex[] = "0123456789ABCDEF";
    embi_sb_append_char(out, '%');
    embi_sb_append_char(out, hex[b >> 4]);
    embi_sb_append_char(out, hex[b & 15]);
}

/* Percent-encodes one code point. */
static void pct_encode_cp(embi_sb *out, uint32_t cp, enum pct_set set)
{
    if (!in_pct_set(cp, set)) {
        embi_sb_append_cp(out, cp);
        return;
    }
    char b[4];
    size_t k = embi_utf8_encode(cp, b);
    for (size_t i = 0; i < k; i++) {
        append_pct_byte(out, (unsigned char)b[i]);
    }
}

/* Percent-encodes bytes. Every byte above 0x7E is in every set, so this is the same as encoding
 * the code points of valid UTF-8. */
static void pct_encode_bytes(embi_sb *out, const char *p, size_t n, enum pct_set set)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)p[i];
        if (in_pct_set(b, set)) {
            append_pct_byte(out, b);
        } else {
            embi_sb_append_char(out, (char)b);
        }
    }
}

/* Percent-encode after encoding for a single-byte query encoding: ASCII goes through the set,
 * a mapped code point is one byte (always percent-encoded, being above 0x7E), and an unmapped
 * one is its "&#N;" reference, percent-encoded. */
static void pct_encode_query_enc(embi_sb *out, const char *p, size_t n, enum pct_set set,
                                 const embi_encoding *enc)
{
    const char *e = n != 0 ? p + n : nullptr;
    while (p != nullptr && p < e) {
        uint32_t cp = embi_utf8_decode(&p, e);
        uint8_t b;
        if (cp < 0x80u) {
            pct_encode_cp(out, cp, set);
        } else if (embi_encode_single(enc, cp, &b)) {
            append_pct_byte(out, b);
        } else {
            embi_sb_append(out, EMBI_STR("%26%23"));
            embi_sb_append_u64(out, cp);
            embi_sb_append(out, EMBI_STR("%3B"));
        }
    }
}

/* Decodes the UTF-8 in s into code points, in order. */
static void sb_to_cps(embi_str s, cpvec *out)
{
    const char *p = s.ptr;
    const char *e = embi_str_end(s);
    while (p < e) {
        embi_vec_push(out, embi_utf8_decode(&p, e));
    }
}

static uint32_t cp_count(const embi_sb *s)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        if (((unsigned char)s->data[i] & 0xC0u) != 0x80u) {
            n++;
        }
    }
    return n;
}

/* Whether a string (ASCII) is a Windows drive letter: a letter and ':' or '|'. */
static bool is_wdl_seg(const char *s, size_t n, bool allow_bar)
{
    if (n != 2 || !is_alpha((unsigned char)s[0])) {
        return false;
    }
    return s[1] == ':' || (allow_bar && s[1] == '|');
}

/* Normalized: the second code point is ':'. */
static bool is_normalized_wdl(const char *s, size_t n)
{
    return is_wdl_seg(s, n, false);
}

/* The code points from ptr on start with a Windows drive letter (spec 4.4). */
static bool starts_with_wdl(const uint32_t *in, uint32_t n, int64_t ptr)
{
    if (ptr < 0 || (uint64_t)ptr >= n) {
        return false;
    }
    uint64_t left = n - (uint64_t)ptr;
    if (left < 2) {
        return false;
    }
    uint32_t c0 = in[ptr];
    uint32_t c1 = in[ptr + 1];
    if (!is_alpha(c0) || (c1 != ':' && c1 != '|')) {
        return false;
    }
    if (left == 2) {
        return true;
    }
    uint32_t c2 = in[ptr + 2];
    return c2 == '/' || c2 == '\\' || c2 == '?' || c2 == '#';
}

/* ------------------------------------------------------------------ path list operations */

static void path_append_seg(embi_sb *path, const char *seg, size_t n)
{
    embi_sb_append_char(path, '/');
    if (n > 0) {
        embi_sb_append(path, (embi_str){seg, (uint32_t)n});
    }
}

/* The first segment of a list path, or false when the path has no segment. */
static bool path_first_seg(const embi_sb *path, const char **seg, size_t *n)
{
    if (path->len == 0 || path->data[0] != '/') {
        return false;
    }
    size_t e = 1;
    while (e < path->len && path->data[e] != '/') {
        e++;
    }
    *seg = path->data + 1;
    *n = e - 1;
    return true;
}

static void path_shorten(urec *u)
{
    if (sb_is(&u->scheme, "file")) {
        const char *seg = nullptr;
        size_t sn = 0;
        if (path_first_seg(&u->path, &seg, &sn) && is_normalized_wdl(seg, sn)) {
            /* Only the drive letter segment: keep it. */
            size_t segments = 0;
            for (uint32_t i = 0; i < u->path.len && segments < 2; i++) {
                if (u->path.data[i] == '/') {
                    segments++;
                }
            }
            if (segments == 1) {
                return;
            }
        }
    }
    if (u->path.len == 0) {
        return;
    }
    uint32_t i = u->path.len;
    while (i > 0 && u->path.data[i - 1] != '/') {
        i--;
    }
    /* u->path.data[i - 1] is the '/' that starts the last segment. */
    u->path.len = i - 1;
}

static bool is_single_dot(const embi_sb *b)
{
    if (b->len == 1 && b->data[0] == '.') {
        return true;
    }
    return b->len == 3 && b->data[0] == '%' && b->data[1] == '2' && (b->data[2] | 0x20) == 'e';
}

static bool is_double_dot(const embi_sb *b)
{
    embi_str s = {b->data, b->len};
    if (b->len == 2 && b->data[0] == '.' && b->data[1] == '.') {
        return true;
    }
    if (b->len == 4 && embi_str_ieq(s, EMBI_STR(".%2e"))) {
        return true;
    }
    if (b->len == 4 && embi_str_ieq(s, EMBI_STR("%2e."))) {
        return true;
    }
    return b->len == 6 && embi_str_ieq(s, EMBI_STR("%2e%2e"));
}

/* ------------------------------------------------------------------ IPv4 and IPv6 */

/* IPv4 number (spec 4.4): returns false on failure. Values are capped so they stay comparable. */
static bool ipv4_number(const char *s, size_t n, uint64_t *out)
{
    if (n == 0) {
        return false;
    }
    unsigned radix = 10;
    if (n >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        n -= 2;
        radix = 16;
    } else if (n >= 2 && s[0] == '0') {
        s += 1;
        n -= 1;
        radix = 8;
    }
    if (n == 0) {
        *out = 0;
        return true;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if ((c | 0x20u) >= 'a' && (c | 0x20u) <= 'f') {
            d = (c | 0x20u) - 'a' + 10;
        } else {
            return false;
        }
        if (d >= radix) {
            return false;
        }
        v = v * radix + d;
        if (v > (1ull << 40)) {
            v = 1ull << 40;
        }
    }
    *out = v;
    return true;
}

/* The last segment of a dotted string, after the empty-last rule, and the segment count. */
static bool ends_in_number(const char *s, size_t n)
{
    size_t seg_end = n;
    if (n > 0 && s[n - 1] == '.') {
        seg_end = n - 1;
    }
    size_t ls = seg_end;
    while (ls > 0 && s[ls - 1] != '.') {
        ls--;
    }
    size_t ll = seg_end - ls;
    if (ll > 0) {
        bool digits = true;
        for (size_t i = 0; i < ll; i++) {
            if (!is_digit((unsigned char)s[ls + i])) {
                digits = false;
                break;
            }
        }
        if (digits) {
            return true;
        }
    }
    uint64_t v;
    return ipv4_number(s + ls, ll, &v);
}

static bool parse_ipv4(const char *s, size_t n, embi_sb *out)
{
    /* Split on '.'. A trailing '.' leaves an empty last part, which is dropped. */
    size_t starts[8];
    size_t lens[8];
    size_t count = 0;
    size_t st = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '.') {
            if (count == 8) {
                return false;
            }
            starts[count] = st;
            lens[count] = i - st;
            count++;
            st = i + 1;
        }
    }
    if (lens[count - 1] == 0 && count > 1) {
        count--;
    }
    if (count > 4) {
        return false;
    }
    uint64_t num[4];
    for (size_t i = 0; i < count; i++) {
        if (!ipv4_number(s + starts[i], lens[i], &num[i])) {
            return false;
        }
    }
    for (size_t i = 0; i + 1 < count; i++) {
        if (num[i] > 255) {
            return false;
        }
    }
    uint64_t limit = 1;
    for (size_t i = 0; i < 5 - count; i++) {
        limit *= 256;
    }
    if (num[count - 1] >= limit) {
        return false;
    }
    uint64_t addr = num[count - 1];
    for (size_t i = 0; i + 1 < count; i++) {
        uint64_t mul = 1;
        for (size_t k = 0; k < 3 - i; k++) {
            mul *= 256;
        }
        addr += num[i] * mul;
    }
    for (int i = 3; i >= 0; i--) {
        uint32_t octet = (uint32_t)((addr >> (8 * i)) & 0xFF);
        char tmp[4];
        int k = 0;
        do {
            tmp[k++] = (char)('0' + octet % 10);
            octet /= 10;
        } while (octet > 0);
        while (k > 0) {
            embi_sb_append_char(out, tmp[--k]);
        }
        if (i > 0) {
            embi_sb_append_char(out, '.');
        }
    }
    return true;
}

/* Parses the inside of [...] (spec 4.4, IPv6 parser) into 8 pieces. */
static bool parse_ipv6(const uint32_t *in, uint32_t n, uint16_t a[8])
{
    for (int i = 0; i < 8; i++) {
        a[i] = 0;
    }
    int pi = 0;
    int compress = -1;
    uint32_t p = 0;
    if (p < n && in[p] == ':') {
        if (!(p + 1 < n && in[p + 1] == ':')) {
            return false;
        }
        p += 2;
        pi++;
        compress = pi;
    }
    while (p < n) {
        if (pi == 8) {
            return false;
        }
        if (in[p] == ':') {
            if (compress != -1) {
                return false;
            }
            p++;
            pi++;
            compress = pi;
            continue;
        }
        uint32_t value = 0;
        uint32_t length = 0;
        while (length < 4 && p < n && is_hex(in[p])) {
            value = value * 16 + hex_value(in[p]);
            p++;
            length++;
        }
        if (p < n && in[p] == '.') {
            if (length == 0) {
                return false;
            }
            p -= length;
            if (pi > 6) {
                return false;
            }
            int numbers_seen = 0;
            while (p < n) {
                int piece = -1;
                if (numbers_seen > 0) {
                    if (in[p] == '.' && numbers_seen < 4) {
                        p++;
                    } else {
                        return false;
                    }
                }
                if (!(p < n && is_digit(in[p]))) {
                    return false;
                }
                while (p < n && is_digit(in[p])) {
                    int number = (int)(in[p] - '0');
                    if (piece == -1) {
                        piece = number;
                    } else if (piece == 0) {
                        return false;
                    } else {
                        piece = piece * 10 + number;
                    }
                    if (piece > 255) {
                        return false;
                    }
                    p++;
                }
                a[pi] = (uint16_t)(a[pi] * 0x100 + piece);
                numbers_seen++;
                if (numbers_seen == 2 || numbers_seen == 4) {
                    pi++;
                }
            }
            if (numbers_seen != 4) {
                return false;
            }
            break;
        }
        if (p < n && in[p] == ':') {
            p++;
            if (p >= n) {
                return false;
            }
        } else if (p < n) {
            return false;
        }
        a[pi] = (uint16_t)value;
        pi++;
    }
    if (compress != -1) {
        int swaps = pi - compress;
        pi = 7;
        while (pi != 0 && swaps > 0) {
            uint16_t t = a[pi];
            a[pi] = a[compress + swaps - 1];
            a[compress + swaps - 1] = t;
            pi--;
            swaps--;
        }
    } else if (pi != 8) {
        return false;
    }
    return true;
}

static void serialize_ipv6(const uint16_t a[8], embi_sb *out)
{
    int best = -1;
    int best_len = 1;
    for (int i = 0; i < 8;) {
        if (a[i] == 0) {
            int j = i;
            while (j < 8 && a[j] == 0) {
                j++;
            }
            if (j - i > best_len) {
                best = i;
                best_len = j - i;
            }
            i = j;
        } else {
            i++;
        }
    }
    static const char hex[] = "0123456789abcdef";
    embi_sb_append_char(out, '[');
    bool ignore0 = false;
    for (int i = 0; i < 8; i++) {
        if (ignore0 && a[i] == 0) {
            continue;
        }
        ignore0 = false;
        if (best == i) {
            embi_sb_append_char(out, ':');
            if (i == 0) {
                embi_sb_append_char(out, ':');
            }
            ignore0 = true;
            continue;
        }
        uint16_t v = a[i];
        char tmp[4];
        int k = 0;
        do {
            tmp[k++] = hex[v & 15];
            v >>= 4;
        } while (v > 0);
        while (k > 0) {
            embi_sb_append_char(out, tmp[--k]);
        }
        if (i != 7) {
            embi_sb_append_char(out, ':');
        }
    }
    embi_sb_append_char(out, ']');
}

/* ------------------------------------------------------------------ host parser */

static bool forbidden_host_cp(uint32_t c)
{
    switch (c) {
    case 0x00: case 0x09: case 0x0A: case 0x0D: case ' ': case '#': case '/': case ':':
    case '<': case '>': case '?': case '@': case '[': case '\\': case ']': case '^': case '|':
        return true;
    default:
        return false;
    }
}

static bool forbidden_domain_byte(unsigned char c)
{
    return forbidden_host_cp(c) || c <= 0x1F || c == '%' || c == 0x7F;
}

/* Host parser (spec 4.4) over code points. Writes the serialized host to out. */
static bool parse_host(const cpvec *in, bool opaque, embi_sb *out)
{
    const uint32_t *s = in->data;
    uint32_t n = in->len;
    if (n > 0 && s[0] == '[') {
        if (n < 2 || s[n - 1] != ']') {
            return false;
        }
        uint16_t a[8];
        if (!parse_ipv6(s + 1, n - 2, a)) {
            return false;
        }
        serialize_ipv6(a, out);
        return true;
    }
    if (opaque) {
        for (uint32_t i = 0; i < n; i++) {
            if (forbidden_host_cp(s[i])) {
                return false;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            pct_encode_cp(out, s[i], PS_C0);
        }
        return true;
    }

    /* Domain: percent-decode the UTF-8 bytes, decode them lossily, then ToASCII. */
    embi_sb utf8 = {nullptr, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        embi_sb_append_cp(&utf8, s[i]);
    }
    embi_sb bytes = {nullptr, 0, 0};
    embi_pct_decode((embi_str){utf8.data, utf8.len}, &bytes);
    embi_sb domain = {nullptr, 0, 0};
    {
        const char *p = bytes.data;
        const char *e = sb_end(&bytes);
        while (p < e) {
            embi_sb_append_cp(&domain, embi_utf8_decode(&p, e));
        }
    }
    embi_sb ascii = {nullptr, 0, 0};
    bool ok = embi_idna_to_ascii((embi_str){domain.data, domain.len}, &ascii) && ascii.len > 0;
    if (ok) {
        for (uint32_t i = 0; i < ascii.len; i++) {
            if (forbidden_domain_byte((unsigned char)ascii.data[i])) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        if (ends_in_number(ascii.data, ascii.len)) {
            ok = parse_ipv4(ascii.data, ascii.len, out);
        } else {
            embi_sb_append(out, (embi_str){ascii.data, ascii.len});
        }
    }
    embi_sb_free(&utf8);
    embi_sb_free(&bytes);
    embi_sb_free(&domain);
    embi_sb_free(&ascii);
    return ok;
}

/* ------------------------------------------------------------------ basic URL parser */

/* Result of a parse step: success, or failure. A state override "return" counts as success. */
typedef struct parser {
    const uint32_t *in;
    uint32_t n;
    int64_t ptr;
    int state;
    int override;
    embi_sb buf;
    bool at_seen;
    bool inside_brackets;
    bool password_token_seen;
    const embi_encoding *qenc; /* single-byte query encoder, or nullptr for UTF-8 */
} parser;

static uint32_t cur_cp(const parser *ps)
{
    if (ps->ptr < 0 || (uint64_t)ps->ptr >= ps->n) {
        return EOFC;
    }
    return ps->in[ps->ptr];
}

static uint32_t cp_at(const parser *ps, int64_t off)
{
    if (ps->ptr + off < 0 || (uint64_t)(ps->ptr + off) >= ps->n) {
        return EOFC;
    }
    return ps->in[ps->ptr + off];
}

static void buf_append_cp(parser *ps, uint32_t c)
{
    embi_sb_append_cp(&ps->buf, c);
}

/* Moves a finished URL's buffer text into the scheme field, lowercased (already ASCII). */
static void buf_to_field(parser *ps, embi_sb *field)
{
    sb_set(field, &ps->buf);
}

/* The state machine. Returns false on failure. On success, *u holds the result (or the partial
 * result, for a state override that returned early, as the setters require). */
static bool basic_parse_raw(const uint32_t *in, uint32_t n, const urec *base, urec *u, int override,
                            const embi_encoding *qenc);

/* The basic URL parser drops ASCII tab and newline from its input in every state, setters included. */
static bool basic_parse(const uint32_t *in, uint32_t n, const urec *base, urec *u, int override,
                        const embi_encoding *qenc)
{
    cpvec clean = {nullptr, 0, 0};
    for (uint32_t i = 0; i < n; i++) {
        if (in[i] != 0x09 && in[i] != 0x0A && in[i] != 0x0D) {
            embi_vec_push(&clean, in[i]);
        }
    }
    bool ok = basic_parse_raw(clean.data, clean.len, base, u, override, qenc);
    embi_vec_free(&clean);
    return ok;
}

static bool basic_parse_raw(const uint32_t *in, uint32_t n, const urec *base, urec *u, int override,
                            const embi_encoding *qenc)
{
    parser ps = {
        .in = in,
        .n = n,
        .ptr = 0,
        .state = override == ST_NONE ? ST_SCHEME_START : override,
        .override = override,
        .buf = {nullptr, 0, 0},
        .at_seen = false,
        .inside_brackets = false,
        .password_token_seen = false,
        .qenc = qenc,
    };
    bool ok = true;
    for (;;) {
        uint32_t c = cur_cp(&ps);
        switch (ps.state) {
        case ST_SCHEME_START:
            if (is_alpha(c)) {
                buf_append_cp(&ps, lower_cp(c));
                ps.state = ST_SCHEME;
            } else if (override == ST_NONE) {
                ps.state = ST_NO_SCHEME;
                ps.ptr--;
            } else {
                ok = false;
                goto done;
            }
            break;

        case ST_SCHEME:
            if (is_alnum(c) || c == '+' || c == '-' || c == '.') {
                buf_append_cp(&ps, lower_cp(c));
            } else if (c == ':') {
                if (override != ST_NONE) {
                    bool buf_special = special_name((const char *)ps.buf.data, ps.buf.len);
                    if (buf_special != urec_special(u)) {
                        goto done;
                    }
                    if ((has_credentials(u) || u->port >= 0) && sb_is(&ps.buf, "file")) {
                        goto done;
                    }
                    if (sb_is(&u->scheme, "file") && u->has_host && u->host.len == 0) {
                        goto done;
                    }
                }
                buf_to_field(&ps, &u->scheme);
                if (override != ST_NONE) {
                    if (u->port == default_port_of(&u->scheme)) {
                        u->port = -1;
                    }
                    goto done;
                }
                ps.buf.len = 0;
                if (sb_is(&u->scheme, "file")) {
                    ps.state = ST_FILE;
                } else if (urec_special(u) && base != nullptr && sb_eq(&base->scheme, &u->scheme)) {
                    ps.state = ST_SPECIAL_RELATIVE_OR_AUTHORITY;
                } else if (urec_special(u)) {
                    ps.state = ST_SPECIAL_AUTHORITY_SLASHES;
                } else if (cp_at(&ps, 1) == '/') {
                    ps.state = ST_PATH_OR_AUTHORITY;
                    ps.ptr++;
                } else {
                    sb_set_str(&u->path, (embi_str){nullptr, 0});
                    u->opaque = true;
                    ps.state = ST_OPAQUE_PATH;
                }
            } else if (override == ST_NONE) {
                ps.buf.len = 0;
                ps.state = ST_NO_SCHEME;
                ps.ptr = -1;
            } else {
                ok = false;
                goto done;
            }
            break;

        case ST_NO_SCHEME:
            if (base == nullptr || (base->opaque && c != '#')) {
                ok = false;
                goto done;
            } else if (base->opaque && c == '#') {
                sb_set(&u->scheme, &base->scheme);
                sb_set(&u->path, &base->path);
                u->opaque = true;
                if (base->has_query) {
                    sb_set(&u->query, &base->query);
                    u->has_query = true;
                }
                u->has_frag = true;
                u->frag.len = 0;
                ps.state = ST_FRAGMENT;
            } else if (!sb_is(&base->scheme, "file")) {
                ps.state = ST_RELATIVE;
                ps.ptr--;
            } else {
                ps.state = ST_FILE;
                ps.ptr--;
            }
            break;

        case ST_SPECIAL_RELATIVE_OR_AUTHORITY:
            if (c == '/' && cp_at(&ps, 1) == '/') {
                ps.state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                ps.ptr++;
            } else {
                ps.state = ST_RELATIVE;
                ps.ptr--;
            }
            break;

        case ST_PATH_OR_AUTHORITY:
            if (c == '/') {
                ps.state = ST_AUTHORITY;
            } else {
                ps.state = ST_PATH;
                ps.ptr--;
            }
            break;

        case ST_RELATIVE:
            sb_set(&u->scheme, &base->scheme);
            if (c == '/') {
                ps.state = ST_RELATIVE_SLASH;
            } else if (urec_special(u) && c == '\\') {
                ps.state = ST_RELATIVE_SLASH;
            } else {
                sb_set(&u->username, &base->username);
                sb_set(&u->password, &base->password);
                sb_set(&u->host, &base->host);
                u->has_host = base->has_host;
                u->port = base->port;
                sb_set(&u->path, &base->path);
                u->opaque = false;
                if (c == '?') {
                    sb_set(&u->query, &base->query);
                    u->has_query = base->has_query;
                    u->query.len = 0;
                    u->has_query = true;
                    ps.state = ST_QUERY;
                } else if (c == '#') {
                    sb_set(&u->query, &base->query);
                    u->has_query = base->has_query;
                    u->has_frag = true;
                    u->frag.len = 0;
                    ps.state = ST_FRAGMENT;
                } else if (c != EOFC) {
                    sb_set(&u->query, &base->query);
                    u->has_query = false;
                    u->query.len = 0;
                    path_shorten(u);
                    ps.state = ST_PATH;
                    ps.ptr--;
                } else {
                    sb_set(&u->query, &base->query);
                    u->has_query = base->has_query;
                }
            }
            break;

        case ST_RELATIVE_SLASH:
            if (urec_special(u) && (c == '/' || c == '\\')) {
                ps.state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
            } else if (c == '/') {
                ps.state = ST_AUTHORITY;
            } else {
                sb_set(&u->username, &base->username);
                sb_set(&u->password, &base->password);
                sb_set(&u->host, &base->host);
                u->has_host = base->has_host;
                u->port = base->port;
                ps.state = ST_PATH;
                ps.ptr--;
            }
            break;

        case ST_SPECIAL_AUTHORITY_SLASHES:
            if (c == '/' && cp_at(&ps, 1) == '/') {
                ps.state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                ps.ptr++;
            } else {
                ps.state = ST_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                ps.ptr--;
            }
            break;

        case ST_SPECIAL_AUTHORITY_IGNORE_SLASHES:
            if (c != '/' && c != '\\') {
                ps.state = ST_AUTHORITY;
                ps.ptr--;
            }
            break;

        case ST_AUTHORITY:
            if (c == '@') {
                if (ps.at_seen) {
                    embi_sb tmp = {nullptr, 0, 0};
                    embi_sb_append(&tmp, EMBI_STR("%40"));
                    embi_sb_append(&tmp, (embi_str){ps.buf.data, ps.buf.len});
                    ps.buf.len = 0;
                    embi_sb_append(&ps.buf, (embi_str){tmp.data, tmp.len});
                    embi_sb_free(&tmp);
                }
                ps.at_seen = true;
                const char *p = ps.buf.data;
                const char *e = sb_end(&ps.buf);
                while (p < e) {
                    uint32_t cp = embi_utf8_decode(&p, e);
                    if (cp == ':' && !ps.password_token_seen) {
                        ps.password_token_seen = true;
                        continue;
                    }
                    pct_encode_cp(ps.password_token_seen ? &u->password : &u->username, cp, PS_USERINFO);
                }
                ps.buf.len = 0;
            } else if (c == EOFC || c == '/' || c == '?' || c == '#' ||
                       (urec_special(u) && c == '\\')) {
                if (ps.at_seen && ps.buf.len == 0) {
                    ok = false;
                    goto done;
                }
                ps.ptr -= (int64_t)cp_count(&ps.buf) + 1;
                ps.buf.len = 0;
                ps.state = ST_HOST;
            } else {
                buf_append_cp(&ps, c);
            }
            break;

        case ST_HOST:
        case ST_HOSTNAME:
            if (override != ST_NONE && sb_is(&u->scheme, "file")) {
                ps.ptr--;
                ps.state = ST_FILE_HOST;
            } else if (c == ':' && !ps.inside_brackets) {
                if (ps.buf.len == 0) {
                    ok = false;
                    goto done;
                }
                if (override == ST_HOSTNAME) {
                    goto done;
                }
                cpvec cps = {nullptr, 0, 0};
                sb_to_cps((embi_str){ps.buf.data, ps.buf.len}, &cps);
                embi_sb host = {nullptr, 0, 0};
                bool parsed = parse_host(&cps, !urec_special(u), &host);
                embi_vec_free(&cps);
                if (!parsed) {
                    embi_sb_free(&host);
                    ok = false;
                    goto done;
                }
                sb_set(&u->host, &host);
                embi_sb_free(&host);
                u->has_host = true;
                ps.buf.len = 0;
                ps.state = ST_PORT;
            } else if (c == EOFC || c == '/' || c == '?' || c == '#' ||
                       (urec_special(u) && c == '\\')) {
                ps.ptr--;
                if (urec_special(u) && ps.buf.len == 0) {
                    ok = false;
                    goto done;
                }
                if (override != ST_NONE && ps.buf.len == 0 && (has_credentials(u) || u->port >= 0)) {
                    goto done;
                }
                cpvec cps = {nullptr, 0, 0};
                sb_to_cps((embi_str){ps.buf.data, ps.buf.len}, &cps);
                embi_sb host = {nullptr, 0, 0};
                bool parsed = parse_host(&cps, !urec_special(u), &host);
                embi_vec_free(&cps);
                if (!parsed) {
                    embi_sb_free(&host);
                    ok = false;
                    goto done;
                }
                sb_set(&u->host, &host);
                embi_sb_free(&host);
                u->has_host = true;
                ps.buf.len = 0;
                ps.state = ST_PATH_START;
                if (override != ST_NONE) {
                    goto done;
                }
            } else {
                if (c == '[') {
                    ps.inside_brackets = true;
                }
                if (c == ']') {
                    ps.inside_brackets = false;
                }
                buf_append_cp(&ps, c);
            }
            break;

        case ST_PORT:
            if (is_digit(c)) {
                buf_append_cp(&ps, c);
            } else if (c == EOFC || c == '/' || c == '?' || c == '#' ||
                       (urec_special(u) && c == '\\') || override != ST_NONE) {
                if (ps.buf.len > 0) {
                    uint32_t port = 0;
                    for (uint32_t i = 0; i < ps.buf.len; i++) {
                        port = port * 10 + (uint32_t)(ps.buf.data[i] - '0');
                        if (port > 65535) {
                            ok = false;
                            goto done;
                        }
                    }
                    u->port = (int32_t)port == default_port_of(&u->scheme) ? -1 : (int32_t)port;
                    ps.buf.len = 0;
                }
                if (override != ST_NONE) {
                    goto done;
                }
                ps.state = ST_PATH_START;
                ps.ptr--;
            } else {
                ok = false;
                goto done;
            }
            break;

        case ST_FILE:
            sb_set_str(&u->scheme, EMBI_STR("file"));
            u->has_host = true; /* spec 4.4 file state: host is the empty string */
            u->host.len = 0;
            if (c == '/' || c == '\\') {
                ps.state = ST_FILE_SLASH;
            } else if (base != nullptr && sb_is(&base->scheme, "file")) {
                sb_set(&u->host, &base->host);
                u->has_host = base->has_host;
                sb_set(&u->path, &base->path);
                u->opaque = false;
                if (c == '?') {
                    sb_set(&u->query, &base->query);
                    u->has_query = true;
                    u->query.len = 0;
                    ps.state = ST_QUERY;
                } else if (c == '#') {
                    sb_set(&u->query, &base->query);
                    u->has_query = base->has_query;
                    u->has_frag = true;
                    u->frag.len = 0;
                    ps.state = ST_FRAGMENT;
                } else if (c != EOFC) {
                    u->has_query = false;
                    u->query.len = 0;
                    if (!starts_with_wdl(in, n, ps.ptr)) {
                        path_shorten(u);
                    } else {
                        u->path.len = 0;
                    }
                    ps.state = ST_PATH;
                    ps.ptr--;
                } else {
                    sb_set(&u->query, &base->query);
                    u->has_query = base->has_query;
                }
            } else {
                ps.state = ST_PATH;
                ps.ptr--;
            }
            break;

        case ST_FILE_SLASH:
            if (c == '/' || c == '\\') {
                ps.state = ST_FILE_HOST;
            } else {
                if (base != nullptr && sb_is(&base->scheme, "file")) {
                    sb_set(&u->host, &base->host);
                    u->has_host = base->has_host;
                    const char *seg = nullptr;
                    size_t sn = 0;
                    if (!starts_with_wdl(in, n, ps.ptr) && path_first_seg(&base->path, &seg, &sn) &&
                        is_normalized_wdl(seg, sn)) {
                        path_append_seg(&u->path, seg, sn);
                    }
                }
                ps.state = ST_PATH;
                ps.ptr--;
            }
            break;

        case ST_FILE_HOST:
            if (c == EOFC || c == '/' || c == '\\' || c == '?' || c == '#') {
                ps.ptr--;
                if (override == ST_NONE && is_wdl_seg((const char *)ps.buf.data, ps.buf.len, true)) {
                    ps.state = ST_PATH;
                } else if (ps.buf.len == 0) {
                    u->has_host = true;
                    u->host.len = 0;
                    if (override != ST_NONE) {
                        goto done;
                    }
                    ps.state = ST_PATH_START;
                } else {
                    cpvec cps = {nullptr, 0, 0};
                    sb_to_cps((embi_str){ps.buf.data, ps.buf.len}, &cps);
                    embi_sb host = {nullptr, 0, 0};
                    bool parsed = parse_host(&cps, false, &host);
                    embi_vec_free(&cps);
                    if (!parsed) {
                        embi_sb_free(&host);
                        ok = false;
                        goto done;
                    }
                    if (host.len == 9 && memcmp(host.data, "localhost", 9) == 0) {
                        host.len = 0;
                    }
                    sb_set(&u->host, &host);
                    embi_sb_free(&host);
                    u->has_host = true;
                    if (override != ST_NONE) {
                        goto done;
                    }
                    ps.buf.len = 0;
                    ps.state = ST_PATH_START;
                }
            } else {
                buf_append_cp(&ps, c);
            }
            break;

        case ST_PATH_START:
            if (urec_special(u)) {
                ps.state = ST_PATH;
                if (c != '/' && c != '\\') {
                    ps.ptr--;
                }
            } else if (override == ST_NONE && c == '?') {
                u->query.len = 0;
                u->has_query = true;
                ps.state = ST_QUERY;
            } else if (override == ST_NONE && c == '#') {
                u->frag.len = 0;
                u->has_frag = true;
                ps.state = ST_FRAGMENT;
            } else if (c != EOFC) {
                ps.state = ST_PATH;
                if (c != '/') {
                    ps.ptr--;
                }
            } else if (override != ST_NONE && !u->has_host) {
                path_append_seg(&u->path, "", 0);
            }
            break;

        case ST_PATH: {
            bool slash_like = c == '/' || (urec_special(u) && c == '\\');
            bool ends = c == EOFC || slash_like || (override == ST_NONE && (c == '?' || c == '#'));
            if (ends) {
                if (is_double_dot(&ps.buf)) {
                    path_shorten(u);
                    if (!slash_like) {
                        path_append_seg(&u->path, "", 0);
                    }
                } else if (is_single_dot(&ps.buf) && !slash_like) {
                    path_append_seg(&u->path, "", 0);
                } else if (!is_single_dot(&ps.buf)) {
                    if (sb_is(&u->scheme, "file") && u->path.len == 0 && is_wdl_seg((const char *)ps.buf.data, ps.buf.len, true) && ps.buf.len == 2) {
                        ps.buf.data[1] = ':';
                    }
                    path_append_seg(&u->path, (const char *)ps.buf.data, ps.buf.len);
                }
                ps.buf.len = 0;
                if (c == '?') {
                    u->query.len = 0;
                    u->has_query = true;
                    ps.state = ST_QUERY;
                }
                if (c == '#') {
                    u->frag.len = 0;
                    u->has_frag = true;
                    ps.state = ST_FRAGMENT;
                }
            } else {
                pct_encode_cp(&ps.buf, c, PS_PATH);
            }
            break;
        }

        case ST_OPAQUE_PATH:
            if (c == '?') {
                u->query.len = 0;
                u->has_query = true;
                ps.state = ST_QUERY;
            } else if (c == '#') {
                u->frag.len = 0;
                u->has_frag = true;
                ps.state = ST_FRAGMENT;
            } else if (c == ' ' && (cp_at(&ps, 1) == '?' || cp_at(&ps, 1) == '#')) {
                /* A space right before a query or fragment is encoded so it survives serialization. */
                embi_sb_append(&u->path, EMBI_STR("%20"));
            } else if (c != EOFC) {
                pct_encode_cp(&u->path, c, PS_C0);
            }
            break;

        case ST_QUERY:
            if ((override == ST_NONE && c == '#') || c == EOFC) {
                enum pct_set set = urec_special(u) ? PS_SPECIAL_QUERY : PS_QUERY;
                if (ps.qenc != nullptr && urec_special(u) && !sb_is(&u->scheme, "ws") &&
                    !sb_is(&u->scheme, "wss")) {
                    pct_encode_query_enc(&u->query, ps.buf.data, ps.buf.len, set, ps.qenc);
                } else {
                    pct_encode_bytes(&u->query, (const char *)ps.buf.data, ps.buf.len, set);
                }
                ps.buf.len = 0;
                if (c == '#') {
                    u->frag.len = 0;
                    u->has_frag = true;
                    ps.state = ST_FRAGMENT;
                }
            } else if (c != EOFC) {
                buf_append_cp(&ps, c);
            }
            break;

        case ST_FRAGMENT:
            if (c != EOFC) {
                pct_encode_cp(&u->frag, c, PS_FRAGMENT);
            }
            break;

        default:
            ok = false;
            goto done;
        }

        if (ps.ptr >= (int64_t)ps.n) {
            break;
        }
        ps.ptr++;
    }

done:
    embi_sb_free(&ps.buf);
    return ok;
}

/* ------------------------------------------------------------------ packing and unpacking */

static void free_href(embi_url *e)
{
    if (e->href != nullptr) {
        embi_free(e->href, (size_t)e->href_len + 1);
    }
    *e = (embi_url){0};
}

/* Serializes a record into the packed form. */
static bool pack(const urec *u, embi_url *out)
{
    embi_sb s = {nullptr, 0, 0};
    embi_url e = {0};
    embi_sb_append(&s, (embi_str){u->scheme.data, u->scheme.len});
    e.scheme_len = u->scheme.len;
    embi_sb_append_char(&s, ':');
    e.has_host = u->has_host;
    if (u->has_host) {
        embi_sb_append(&s, EMBI_STR("//"));
        if (has_credentials(u)) {
            e.user_off = s.len;
            e.user_len = u->username.len;
            embi_sb_append(&s, (embi_str){u->username.data, u->username.len});
            if (u->password.len > 0) {
                embi_sb_append_char(&s, ':');
                e.pass_off = s.len;
                e.pass_len = u->password.len;
                embi_sb_append(&s, (embi_str){u->password.data, u->password.len});
            }
            embi_sb_append_char(&s, '@');
        }
        e.host_off = s.len;
        e.host_len = u->host.len;
        embi_sb_append(&s, (embi_str){u->host.data, u->host.len});
        if (u->port >= 0) {
            embi_sb_append_char(&s, ':');
            embi_sb_append_u64(&s, (uint64_t)u->port);
        }
    } else if (!u->opaque && u->path.len >= 2 && u->path.data[0] == '/' && u->path.data[1] == '/') {
        embi_sb_append(&s, EMBI_STR("/."));
    }
    e.path_off = s.len;
    e.path_len = u->path.len;
    e.opaque_path = u->opaque;
    embi_sb_append(&s, (embi_str){u->path.data, u->path.len});
    if (u->has_query) {
        embi_sb_append_char(&s, '?');
        e.has_query = true;
        e.query_off = s.len;
        e.query_len = u->query.len;
        embi_sb_append(&s, (embi_str){u->query.data, u->query.len});
    }
    if (u->has_frag) {
        embi_sb_append_char(&s, '#');
        e.has_fragment = true;
        e.frag_off = s.len;
        e.frag_len = u->frag.len;
        embi_sb_append(&s, (embi_str){u->frag.data, u->frag.len});
    }
    e.port = u->port;

    char *h = embi_alloc((size_t)s.len + 1);
    if (s.len > 0) {
        memcpy(h, s.data, s.len);
    }
    h[s.len] = '\0';
    e.href = h;
    e.href_len = s.len;
    embi_sb_free(&s);
    *out = e;
    return true;
}

static void unpack(const embi_url *e, urec *u)
{
    urec_init(u);
    if (e->href == nullptr) {
        return; /* a zeroed embi_url */
    }
    const char *h = e->href;
    sb_set_str(&u->scheme, (embi_str){h, e->scheme_len});
    if (e->has_host) {
        sb_set_str(&u->username, (embi_str){h + e->user_off, e->user_len});
        sb_set_str(&u->password, (embi_str){h + e->pass_off, e->pass_len});
        sb_set_str(&u->host, (embi_str){h + e->host_off, e->host_len});
        u->has_host = true;
    }
    u->port = e->port;
    sb_set_str(&u->path, (embi_str){h + e->path_off, e->path_len});
    u->opaque = e->opaque_path;
    if (e->has_query) {
        sb_set_str(&u->query, (embi_str){h + e->query_off, e->query_len});
        u->has_query = true;
    }
    if (e->has_fragment) {
        sb_set_str(&u->frag, (embi_str){h + e->frag_off, e->frag_len});
        u->has_frag = true;
    }
}

/* ------------------------------------------------------------------ public API */

void embi_url_free(embi_url *u)
{
    free_href(u);
}

bool embi_url_copy(const embi_url *src, embi_url *dst)
{
    *dst = (embi_url){0};
    if (src->href == nullptr) {
        return true;
    }
    char *h = embi_alloc((size_t)src->href_len + 1);
    memcpy(h, src->href, (size_t)src->href_len + 1);
    *dst = *src;
    dst->href = h;
    return true;
}

bool embi_url_parse(embi_str input, const embi_url *base, embi_url *out)
{
    return embi_url_parse_enc(input, base, nullptr, out);
}

bool embi_url_parse_enc(embi_str input, const embi_url *base, const embi_encoding *query_enc, embi_url *out)
{
    if (query_enc != nullptr && query_enc->kind != EMBI_ENC_SINGLE && query_enc->kind != EMBI_ENC_USER_DEFINED) {
        query_enc = nullptr;
    }
    *out = (embi_url){0};
    if (base != nullptr && base->href == nullptr) {
        return false; /* a base that was never parsed */
    }
    urec b;
    urec_init(&b);
    if (base != nullptr) {
        unpack(base, &b);
    }

    /* Strip leading and trailing C0 control or space, then drop tabs and newlines. */
    cpvec cps = {nullptr, 0, 0};
    sb_to_cps(input, &cps);
    uint32_t first = 0;
    uint32_t last = cps.len;
    while (first < last && cps.data[first] <= 0x20) {
        first++;
    }
    while (last > first && cps.data[last - 1] <= 0x20) {
        last--;
    }
    cpvec clean = {nullptr, 0, 0};
    for (uint32_t i = first; i < last; i++) {
        uint32_t c = cps.data[i];
        if (c != 0x09 && c != 0x0A && c != 0x0D) {
            embi_vec_push(&clean, c);
        }
    }
    embi_vec_free(&cps);

    urec u;
    urec_init(&u);
    bool ok = basic_parse(clean.data, clean.len, base != nullptr ? &b : nullptr, &u, ST_NONE, query_enc);
    embi_vec_free(&clean);
    if (ok) {
        ok = pack(&u, out);
    }
    urec_free(&u);
    urec_free(&b);
    return ok;
}

bool embi_url_can_parse(embi_str input, const embi_url *base)
{
    embi_url u = {0};
    bool ok = embi_url_parse(input, base, &u);
    embi_url_free(&u);
    return ok;
}

bool embi_url_is_special(const embi_url *u)
{
    return special_name(u->href, u->scheme_len);
}

/* The origin tuple of a URL (spec 4.6: the origin of a URL). Returns false for opaque origins. */
static void origin_of(const urec *u, embi_sb *out)
{
    if (sb_is(&u->scheme, "blob")) {
        embi_url inner = {0};
        embi_sb path = {nullptr, 0, 0};
        embi_sb_append(&path, (embi_str){u->path.data, u->path.len});
        if (embi_url_parse((embi_str){path.data, path.len}, nullptr, &inner)) {
            if (embi_str_ieq((embi_str){inner.href, inner.scheme_len}, EMBI_STR("http")) ||
                embi_str_ieq((embi_str){inner.href, inner.scheme_len}, EMBI_STR("https"))) {
                embi_url_origin(&inner, out);
                embi_url_free(&inner);
                embi_sb_free(&path);
                return;
            }
        }
        embi_url_free(&inner);
        embi_sb_free(&path);
        embi_sb_append(out, EMBI_STR("null"));
        return;
    }
    /* app (W7d): the embedder's scheme for a bundled app. Every app:// response is same-origin (W7 out of scope: CORS),
     * so its origin is a tuple, which localStorage and the other per-origin storage need. */
    if (sb_is(&u->scheme, "ftp") || sb_is(&u->scheme, "http") || sb_is(&u->scheme, "https") ||
        sb_is(&u->scheme, "ws") || sb_is(&u->scheme, "wss") || sb_is(&u->scheme, "app")) {
        embi_sb_append(out, (embi_str){u->scheme.data, u->scheme.len});
        embi_sb_append(out, EMBI_STR("://"));
        embi_sb_append(out, (embi_str){u->host.data, u->host.len});
        if (u->port >= 0) {
            embi_sb_append_char(out, ':');
            embi_sb_append_u64(out, (uint64_t)u->port);
        }
        return;
    }
    embi_sb_append(out, EMBI_STR("null"));
}

void embi_url_origin(const embi_url *e, embi_sb *out)
{
    urec u;
    unpack(e, &u);
    origin_of(&u, out);
    urec_free(&u);
}

void embi_url_get(const embi_url *e, embi_url_attr attr, embi_sb *out)
{
    urec u;
    unpack(e, &u);
    switch (attr) {
    case EMBI_URL_HREF:
        embi_sb_append(out, (embi_str){e->href, e->href_len});
        break;
    case EMBI_URL_PROTOCOL:
        embi_sb_append(out, (embi_str){u.scheme.data, u.scheme.len});
        embi_sb_append_char(out, ':');
        break;
    case EMBI_URL_USERNAME:
        embi_sb_append(out, (embi_str){u.username.data, u.username.len});
        break;
    case EMBI_URL_PASSWORD:
        embi_sb_append(out, (embi_str){u.password.data, u.password.len});
        break;
    case EMBI_URL_HOST:
    case EMBI_URL_HOSTNAME:
        if (u.has_host) {
            embi_sb_append(out, (embi_str){u.host.data, u.host.len});
            if (attr == EMBI_URL_HOST && u.port >= 0) {
                embi_sb_append_char(out, ':');
                embi_sb_append_u64(out, (uint64_t)u.port);
            }
        }
        break;
    case EMBI_URL_PORT:
        if (u.port >= 0) {
            embi_sb_append_u64(out, (uint64_t)u.port);
        }
        break;
    case EMBI_URL_PATHNAME:
        embi_sb_append(out, (embi_str){u.path.data, u.path.len});
        break;
    case EMBI_URL_SEARCH:
        if (u.has_query && u.query.len > 0) {
            embi_sb_append_char(out, '?');
            embi_sb_append(out, (embi_str){u.query.data, u.query.len});
        }
        break;
    case EMBI_URL_HASH:
        if (u.has_frag && u.frag.len > 0) {
            embi_sb_append_char(out, '#');
            embi_sb_append(out, (embi_str){u.frag.data, u.frag.len});
        }
        break;
    }
    urec_free(&u);
}

/* Replaces the packed URL with the record's serialization. Returns false if packing fails. */
static bool replace_with(embi_url *e, urec *u)
{
    embi_url fresh = {0};
    if (!pack(u, &fresh)) {
        return false;
    }
    free_href(e);
    *e = fresh;
    return true;
}

bool embi_url_set(embi_url *e, embi_url_attr attr, embi_str value)
{
    urec u;
    unpack(e, &u);
    cpvec cps = {nullptr, 0, 0};
    bool ok = true;
    switch (attr) {
    case EMBI_URL_HREF: {
        embi_url parsed = {0};
        if (!embi_url_parse(value, nullptr, &parsed)) {
            urec_free(&u);
            return false;
        }
        embi_url_free(e);
        *e = parsed;
        urec_free(&u);
        return true;
    }
    case EMBI_URL_PROTOCOL:
        sb_to_cps(value, &cps);
        embi_vec_push(&cps, ':');
        ok = basic_parse(cps.data, cps.len, nullptr, &u, ST_SCHEME_START, nullptr);
        break;
    case EMBI_URL_USERNAME:
        if (!cannot_have_userinfo(&u)) {
            embi_sb enc = {nullptr, 0, 0};
            pct_encode_bytes(&enc, value.ptr, value.len, PS_USERINFO);
            embi_sb_free(&u.username);
            u.username = enc;
        }
        break;
    case EMBI_URL_PASSWORD:
        if (!cannot_have_userinfo(&u)) {
            embi_sb enc = {nullptr, 0, 0};
            pct_encode_bytes(&enc, value.ptr, value.len, PS_USERINFO);
            embi_sb_free(&u.password);
            u.password = enc;
        }
        break;
    case EMBI_URL_HOST:
    case EMBI_URL_HOSTNAME:
        if (!u.opaque) {
            sb_to_cps(value, &cps);
            ok = basic_parse(cps.data, cps.len, nullptr, &u,
                             attr == EMBI_URL_HOST ? ST_HOST : ST_HOSTNAME, nullptr);
        }
        break;
    case EMBI_URL_PORT:
        if (!cannot_have_userinfo(&u)) {
            if (value.len == 0) {
                u.port = -1;
            } else {
                sb_to_cps(value, &cps);
                ok = basic_parse(cps.data, cps.len, nullptr, &u, ST_PORT, nullptr);
            }
        }
        break;
    case EMBI_URL_PATHNAME:
        if (!u.opaque) {
            u.path.len = 0;
            sb_to_cps(value, &cps);
            ok = basic_parse(cps.data, cps.len, nullptr, &u, ST_PATH_START, nullptr);
        }
        break;
    case EMBI_URL_SEARCH:
        if (value.len == 0) {
            u.query.len = 0;
            u.has_query = false;
        } else {
            if (value.ptr[0] == '?') {
                value.ptr++;
                value.len--;
            }
            sb_to_cps(value, &cps);
            u.query.len = 0;
            u.has_query = true;
            ok = basic_parse(cps.data, cps.len, nullptr, &u, ST_QUERY, nullptr);
        }
        break;
    case EMBI_URL_HASH:
        if (value.len == 0) {
            u.frag.len = 0;
            u.has_frag = false;
        } else {
            if (value.ptr[0] == '#') {
                value.ptr++;
                value.len--;
            }
            sb_to_cps(value, &cps);
            u.frag.len = 0;
            u.has_frag = true;
            ok = basic_parse(cps.data, cps.len, nullptr, &u, ST_FRAGMENT, nullptr);
        }
        break;
    }
    embi_vec_free(&cps);
    /* The setters ignore a failed parse (the URL keeps its prior state, as the standard
     * requires), so only the packing result matters here. */
    (void)ok;
    bool packed = replace_with(e, &u);
    urec_free(&u);
    return packed;
}

/* ------------------------------------------------------------------ application/x-www-form-urlencoded */

static void append_lossy(embi_sb *out, const embi_sb *bytes)
{
    const char *p = bytes->data;
    const char *e = sb_end(bytes);
    while (p < e) {
        embi_sb_append_cp(out, embi_utf8_decode(&p, e));
    }
}

void embi_pct_decode(embi_str in, embi_sb *out)
{
    for (uint32_t i = 0; i < in.len; i++) {
        unsigned char c = (unsigned char)in.ptr[i];
        if (c == '%' && i + 2 < in.len && is_hex((unsigned char)in.ptr[i + 1]) &&
            is_hex((unsigned char)in.ptr[i + 2])) {
            embi_sb_append_char(out, (char)(hex_value((unsigned char)in.ptr[i + 1]) * 16 +
                                            hex_value((unsigned char)in.ptr[i + 2])));
            i += 2;
        } else {
            embi_sb_append_char(out, (char)c);
        }
    }
}

void embi_form_parse(embi_str input, embi_form *out)
{
    for (uint32_t s = 0; s < input.len;) {
        uint32_t e = s;
        while (e < input.len && input.ptr[e] != '&') {
            e++;
        }
        if (e > s) {
            uint32_t eq = s;
            while (eq < e && input.ptr[eq] != '=') {
                eq++;
            }
            embi_str name = {input.ptr + s, eq - s};
            embi_str value = {nullptr, 0};
            if (eq < e) {
                value = (embi_str){input.ptr + eq + 1, e - eq - 1};
            }
            embi_form_pair pair = {{nullptr, 0, 0}, {nullptr, 0, 0}};
            embi_sb nb = {nullptr, 0, 0};
            embi_sb vb = {nullptr, 0, 0};
            for (uint32_t i = 0; i < name.len; i++) {
                embi_sb_append_char(&nb, name.ptr[i] == '+' ? ' ' : name.ptr[i]);
            }
            for (uint32_t i = 0; i < value.len; i++) {
                embi_sb_append_char(&vb, value.ptr[i] == '+' ? ' ' : value.ptr[i]);
            }
            embi_sb dn = {nullptr, 0, 0};
            embi_sb dv = {nullptr, 0, 0};
            embi_pct_decode((embi_str){nb.data, nb.len}, &dn);
            embi_pct_decode((embi_str){vb.data, vb.len}, &dv);
            append_lossy(&pair.name, &dn);
            append_lossy(&pair.value, &dv);
            embi_sb_free(&nb);
            embi_sb_free(&vb);
            embi_sb_free(&dn);
            embi_sb_free(&dv);
            embi_vec_push(out, pair);
        }
        s = e + 1;
    }
}

/* The byte serializer: alphanumerics and *-._ stay, space becomes '+', the rest is %XX. */
static void urlencode_bytes(embi_sb *out, const embi_sb *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < s->len; i++) {
        unsigned char b = (unsigned char)s->data[i];
        if (is_alnum(b) || b == '*' || b == '-' || b == '.' || b == '_') {
            embi_sb_append_char(out, (char)b);
        } else if (b == ' ') {
            embi_sb_append_char(out, '+');
        } else {
            embi_sb_append_char(out, '%');
            embi_sb_append_char(out, hex[b >> 4]);
            embi_sb_append_char(out, hex[b & 15]);
        }
    }
}

void embi_form_serialize(const embi_form *f, embi_sb *out)
{
    for (uint32_t i = 0; i < f->len; i++) {
        if (i > 0) {
            embi_sb_append_char(out, '&');
        }
        urlencode_bytes(out, &f->data[i].name);
        embi_sb_append_char(out, '=');
        urlencode_bytes(out, &f->data[i].value);
    }
}

void embi_form_free(embi_form *f)
{
    for (uint32_t i = 0; i < f->len; i++) {
        embi_sb_free(&f->data[i].name);
        embi_sb_free(&f->data[i].value);
    }
    embi_vec_free(f);
}
