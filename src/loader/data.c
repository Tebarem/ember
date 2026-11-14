/*
 * The Fetch standard's data: URL processor. Replaces the RFC 2397 helper that used to live in
 * src/embed/url.c (W7d switches the embedder over to this).
 */
#include "loader/loader.h"

#include <string.h>

static const char default_mime[] = "text/plain;charset=US-ASCII";

static bool is_ascii_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool embi_data_url(embi_str url, embi_sb *mime_out, embi_sb *body_out)
{
    embi_url u = {0};
    if (!embi_url_parse(url, nullptr, &u)) {
        return false;
    }
    bool ok = false;
    if (u.scheme_len != 4 || memcmp(u.href, "data", 4) != 0) {
        goto done;
    }

    /* The URL without its fragment, minus the "data:" scheme. */
    uint32_t end = u.has_fragment ? u.frag_off - 1 : u.href_len;
    if (end < 5) {
        goto done;
    }
    const char *s = u.href + 5;
    uint32_t n = end - 5;

    /* The first comma splits the MIME type from the body. No comma is a failure. */
    uint32_t comma = 0;
    while (comma < n && s[comma] != ',') {
        comma++;
    }
    if (comma == n) {
        goto done;
    }
    embi_str mime_part = {s, comma};
    embi_str body_part = {s + comma + 1, n - comma - 1};

    /* Percent-decode the body into bytes. */
    embi_sb body = {nullptr, 0, 0};
    embi_pct_decode(body_part, &body);

    /* The MIME type, with leading and trailing ASCII whitespace removed. */
    while (mime_part.len > 0 && is_ascii_space((unsigned char)mime_part.ptr[0])) {
        mime_part.ptr++;
        mime_part.len--;
    }
    while (mime_part.len > 0 && is_ascii_space((unsigned char)mime_part.ptr[mime_part.len - 1])) {
        mime_part.len--;
    }

    /* A trailing "base64" (ASCII case-insensitive) preceded by ';' and any number of U+0020
     * selects base64; the MIME type is cut at that ';'. */
    bool base64 = false;
    {
        embi_str t = mime_part;
        while (t.len > 0 && t.ptr[t.len - 1] == ' ') {
            t.len--;
        }
        if (t.len >= 6 && embi_str_ieq((embi_str){t.ptr + t.len - 6, 6}, EMBI_STR("base64"))) {
            uint32_t k = t.len - 6;
            while (k > 0 && t.ptr[k - 1] == ' ') {
                k--;
            }
            if (k > 0 && t.ptr[k - 1] == ';') {
                base64 = true;
                mime_part.len = k - 1;
            }
        }
    }

    /* A MIME type that starts with ';' (parameters only) gets text/plain in front. */
    embi_sb typed = {nullptr, 0, 0};
    if (mime_part.len > 0 && mime_part.ptr[0] == ';') {
        embi_sb_append(&typed, EMBI_STR("text/plain"));
        embi_sb_append(&typed, mime_part);
        mime_part = (embi_str){typed.data, typed.len};
    }
    embi_mime mime = {0};
    bool parsed = mime_part.len > 0 && embi_mime_parse(mime_part, &mime);
    if (!parsed) {
        embi_mime_free(&mime);
        embi_sb_append(mime_out, EMBI_STR(default_mime));
    } else {
        embi_mime_serialize(&mime, mime_out);
        embi_mime_free(&mime);
    }
    embi_sb_free(&typed);

    if (base64) {
        /* Forgiving base64 over the percent-decoded body. A failure fails the whole URL. */
        embi_sb decoded = {nullptr, 0, 0};
        bool good = embi_base64_decode((embi_str){body.data, body.len}, &decoded);
        embi_sb_free(&body);
        if (!good) {
            embi_sb_free(&decoded);
            goto done;
        }
        embi_sb_append(body_out, (embi_str){decoded.data, decoded.len});
        embi_sb_free(&decoded);
    } else {
        embi_sb_append(body_out, (embi_str){body.data, body.len});
        embi_sb_free(&body);
    }
    ok = true;

done:
    embi_url_free(&u);
    return ok;
}
