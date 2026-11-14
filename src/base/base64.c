#include "base/base.h"

/* Forgiving-base64 (WHATWG Infra standard) and the standard encoder. The decoder does not
 * allocate beyond the output buffer and the filtered copy of the input. */

static const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static bool is_ascii_ws(unsigned char c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

bool embi_base64_decode(embi_str in, embi_sb *out)
{
    /* Step 1 and 2: remove ASCII whitespace, then strip up to two trailing '=' when the length
     * is a multiple of four. */
    embi_sb data = {nullptr, 0, 0};
    for (uint32_t i = 0; i < in.len; i++) {
        unsigned char c = (unsigned char)in.ptr[i];
        if (!is_ascii_ws(c)) {
            embi_sb_append_char(&data, (char)c);
        }
    }
    uint32_t n = data.len;
    if (n % 4 == 0 && n > 0) {
        if (data.data[n - 1] == '=') {
            n--;
            if (data.data[n - 1] == '=') {
                n--;
            }
        }
    }
    bool ok = n % 4 != 1;
    for (uint32_t i = 0; ok && i < n; i++) {
        if (b64_value((unsigned char)data.data[i]) < 0) {
            ok = false;
        }
    }
    if (!ok) {
        embi_sb_free(&data);
        return false;
    }

    uint32_t acc = 0;
    int bits = 0;
    for (uint32_t i = 0; i < n; i++) {
        acc = (acc << 6) | (uint32_t)b64_value((unsigned char)data.data[i]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            embi_sb_append_char(out, (char)((acc >> bits) & 0xFFu));
        }
    }
    embi_sb_free(&data);
    return true;
}

void embi_base64_encode(const unsigned char *p, size_t n, embi_sb *out)
{
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        embi_sb_append_char(out, b64_alphabet[(v >> 18) & 63]);
        embi_sb_append_char(out, b64_alphabet[(v >> 12) & 63]);
        embi_sb_append_char(out, b64_alphabet[(v >> 6) & 63]);
        embi_sb_append_char(out, b64_alphabet[v & 63]);
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        embi_sb_append_char(out, b64_alphabet[(v >> 18) & 63]);
        embi_sb_append_char(out, b64_alphabet[(v >> 12) & 63]);
        embi_sb_append_char(out, '=');
        embi_sb_append_char(out, '=');
    } else if (n - i == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        embi_sb_append_char(out, b64_alphabet[(v >> 18) & 63]);
        embi_sb_append_char(out, b64_alphabet[(v >> 12) & 63]);
        embi_sb_append_char(out, b64_alphabet[(v >> 6) & 63]);
        embi_sb_append_char(out, '=');
    }
}
