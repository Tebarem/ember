/*
 * HTTP/1.1 response parsing (N1): the status line and headers, the message framing of RFC 9112 section 6, and the
 * chunked decoder. Request serialization is in net.c (it needs the request's state).
 */
#include "net_int.h"

#define HEAD_MAX 65536u
#define TRAILER_MAX 65536u

void embi_http_head_free(embi_http_head *h)
{
    embi_net_mfree(h->reason);
    embi_headers_free(&h->headers);
    *h = (embi_http_head){0};
}

static bool tchar(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return true;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

embi_http_parse embi_http_parse_head(const uint8_t *buf, size_t len, size_t *used, embi_http_head *out)
{
    /* The head ends at the first empty line. A bare LF is a protocol error. */
    size_t end = 0;
    bool found = false;
    size_t scan = len > HEAD_MAX + 4 ? HEAD_MAX + 4 : len;
    for (size_t i = 0; i + 3 < scan; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = i + 4;
            found = true;
            break;
        }
    }
    if (!found) {
        return len > HEAD_MAX ? EMBI_HTTP_BAD : EMBI_HTTP_NEED_MORE;
    }
    if (end > HEAD_MAX) {
        return EMBI_HTTP_BAD;
    }
    *out = (embi_http_head){0};

    const char *p = (const char *)buf;
    size_t pos = 0;
    /* Status line: HTTP/1.x SP 3DIGIT [SP reason] */
    size_t eol = 0;
    while (eol + 1 < end && !(p[eol] == '\r' && p[eol + 1] == '\n')) {
        if (p[eol] == '\n') {
            return EMBI_HTTP_BAD;
        }
        eol++;
    }
    if (eol < 12 || memcmp(p, "HTTP/1.", 7) != 0 || p[7] < '0' || p[7] > '9' || p[8] != ' ') {
        return EMBI_HTTP_BAD;
    }
    out->minor = p[7] - '0';
    for (int i = 0; i < 3; i++) {
        char c = p[9 + i];
        if (c < '0' || c > '9') {
            return EMBI_HTTP_BAD;
        }
        out->status = (uint16_t)(out->status * 10 + (c - '0'));
    }
    if (out->status < 100 || (eol > 12 && p[12] != ' ')) {
        return EMBI_HTTP_BAD;
    }
    size_t rl = eol > 13 ? eol - 13 : 0;
    out->reason = embi_net_strndup(p + (rl ? 13 : eol), rl);
    pos = eol + 2;

    while (pos + 2 < end) { /* the last two bytes are the blank line's CRLF */
        size_t le = pos;
        while (le + 1 < end && !(p[le] == '\r' && p[le + 1] == '\n')) {
            if (p[le] == '\n') {
                embi_http_head_free(out);
                return EMBI_HTTP_BAD;
            }
            le++;
        }
        if (p[pos] == ' ' || p[pos] == '\t') { /* obsolete line folding */
            embi_http_head_free(out);
            return EMBI_HTTP_BAD;
        }
        size_t colon = pos;
        while (colon < le && p[colon] != ':') {
            colon++;
        }
        bool ok = colon > pos && colon < le;
        for (size_t i = pos; ok && i < colon; i++) {
            ok = tchar((unsigned char)p[i]);
        }
        if (!ok) {
            embi_http_head_free(out);
            return EMBI_HTTP_BAD;
        }
        embi_sb value = {0};
        embi_header_value_from_wire(embi_header_trim((embi_str){p + colon + 1, (uint32_t)(le - colon - 1)}), &value);
        embi_headers_add_raw(&out->headers, (embi_str){p + pos, (uint32_t)(colon - pos)}, (embi_str){value.data, value.len});
        embi_sb_free(&value);
        pos = le + 2;
    }
    *used = end;
    return EMBI_HTTP_DONE;
}

bool embi_http_has_token(embi_str value, embi_str token)
{
    uint32_t i = 0;
    while (i < value.len) {
        uint32_t b = i;
        while (i < value.len && value.ptr[i] != ',') {
            i++;
        }
        embi_str item = embi_header_trim((embi_str){value.ptr + b, i - b});
        if (embi_str_ieq(item, token)) {
            return true;
        }
        i++;
    }
    return false;
}

bool embi_http_body_kind(const embi_http_head *h, bool head_request, embi_body_kind *kind, uint64_t *length)
{
    *length = 0;
    if (head_request || h->status < 200 || h->status == 204 || h->status == 304) {
        *kind = EMBI_BODY_NONE;
        return true;
    }
    /* Every Transfer-Encoding line counts, as one comma-separated list (RFC 9112 6.1). chunked, when present, must be
     * the last coding and appear once; a list without chunked is read to the end of the connection. */
    bool have_te = false, chunked_last = false;
    uint32_t chunked_count = 0;
    for (uint32_t i = 0; i < h->headers.list.len; i++) {
        const embi_kv *kv = &h->headers.list.data[i];
        if (!embi_str_ieq((embi_str){kv->name.data, kv->name.len}, EMBI_STR("transfer-encoding"))) {
            continue;
        }
        have_te = true;
        uint32_t k = 0;
        while (k < kv->value.len) {
            uint32_t b = k;
            while (k < kv->value.len && kv->value.data[k] != ',') {
                k++;
            }
            embi_str item = embi_header_trim((embi_str){kv->value.data + b, k - b});
            k++;
            if (item.len == 0) {
                continue;
            }
            chunked_last = embi_str_ieq(item, EMBI_STR("chunked"));
            if (chunked_last) {
                chunked_count++;
            }
        }
    }
    if (have_te) {
        if (chunked_count > 1 || (chunked_count == 1 && !chunked_last)) {
            return false;
        }
        *kind = chunked_count == 1 ? EMBI_BODY_CHUNKED : EMBI_BODY_CLOSE;
        return true;
    }
    bool have = false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < h->headers.list.len; i++) {
        const embi_kv *kv = &h->headers.list.data[i];
        if (!embi_str_ieq((embi_str){kv->name.data, kv->name.len}, EMBI_STR("content-length"))) {
            continue;
        }
        uint32_t k = 0;
        while (k < kv->value.len) { /* a list "5, 5" is allowed when all agree */
            uint32_t b = k;
            while (k < kv->value.len && kv->value.data[k] != ',') {
                k++;
            }
            embi_str item = embi_header_trim((embi_str){kv->value.data + b, k - b});
            k++;
            if (item.len == 0 || item.len > 19) {
                return false;
            }
            uint64_t n = 0;
            for (uint32_t d = 0; d < item.len; d++) {
                if (item.ptr[d] < '0' || item.ptr[d] > '9') {
                    return false;
                }
                n = n * 10 + (uint64_t)(item.ptr[d] - '0');
            }
            if (have && n != v) {
                return false;
            }
            have = true;
            v = n;
        }
    }
    if (have) {
        *kind = EMBI_BODY_LENGTH;
        *length = v;
        return true;
    }
    *kind = EMBI_BODY_CLOSE;
    return true;
}

/* ------------------------------------------------------------------ body framing */

enum {
    CH_SIZE,
    CH_SIZE_FIRST,
    CH_EXT,
    CH_SIZE_LF,
    CH_DATA,
    CH_DATA_CR,
    CH_DATA_LF,
    CH_TRAILER_START,
    CH_TRAILER_LINE,
    CH_TRAILER_LF,
    CH_END_LF,
};

void embi_http_body_init(embi_http_body *b, embi_body_kind kind, uint64_t length)
{
    *b = (embi_http_body){0};
    b->kind = kind;
    b->remaining = length;
    b->state = CH_SIZE_FIRST;
    b->done = kind == EMBI_BODY_NONE || (kind == EMBI_BODY_LENGTH && length == 0);
}

static int hexval(uint8_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

size_t embi_http_body_feed(embi_http_body *b, const uint8_t *in, size_t len, embi_buf *out)
{
    size_t i = 0;
    if (b->done || b->bad) {
        return 0;
    }
    switch (b->kind) {
    case EMBI_BODY_NONE:
        b->done = true;
        return 0;
    case EMBI_BODY_CLOSE:
        (void)embi_buf_append(out, in, len);
        return len;
    case EMBI_BODY_LENGTH: {
        size_t n = len;
        if ((uint64_t)n > b->remaining) {
            n = (size_t)b->remaining;
        }
        (void)embi_buf_append(out, in, n);
        b->remaining -= n;
        b->done = b->remaining == 0;
        return n;
    }
    case EMBI_BODY_CHUNKED:
        break;
    }
    while (i < len && !b->done && !b->bad) {
        uint8_t c = in[i];
        switch (b->state) {
        case CH_SIZE_FIRST:
        case CH_SIZE: {
            int hv = hexval(c);
            if (hv >= 0) {
                if (b->size_acc > (UINT64_MAX >> 5)) {
                    b->bad = true;
                    break;
                }
                b->size_acc = b->size_acc * 16 + (uint64_t)hv;
                b->state = CH_SIZE;
                i++;
            } else if (b->state == CH_SIZE && c == ';') {
                b->state = CH_EXT;
                i++;
            } else if (b->state == CH_SIZE && c == '\r') {
                b->state = CH_SIZE_LF;
                i++;
            } else {
                b->bad = true;
            }
            break;
        }
        case CH_EXT:
            if (c == '\r') {
                b->state = CH_SIZE_LF;
            } else if (c == '\n') {
                b->bad = true;
            }
            i++;
            break;
        case CH_SIZE_LF:
            if (c != '\n') {
                b->bad = true;
                break;
            }
            i++;
            if (b->size_acc == 0) {
                b->state = CH_TRAILER_START;
            } else {
                b->remaining = b->size_acc;
                b->state = CH_DATA;
            }
            b->size_acc = 0;
            break;
        case CH_DATA: {
            size_t n = len - i;
            if ((uint64_t)n > b->remaining) {
                n = (size_t)b->remaining;
            }
            (void)embi_buf_append(out, in + i, n);
            i += n;
            b->remaining -= n;
            if (b->remaining == 0) {
                b->state = CH_DATA_CR;
            }
            break;
        }
        case CH_DATA_CR:
            if (c != '\r') {
                b->bad = true;
                break;
            }
            b->state = CH_DATA_LF;
            i++;
            break;
        case CH_DATA_LF:
            if (c != '\n') {
                b->bad = true;
                break;
            }
            b->state = CH_SIZE_FIRST;
            i++;
            break;
        case CH_TRAILER_START:
            if (c == '\r') {
                b->state = CH_END_LF;
            } else {
                b->state = CH_TRAILER_LINE;
                continue; /* reprocess in the line state */
            }
            i++;
            break;
        case CH_TRAILER_LINE:
            if (++b->trailer_bytes > TRAILER_MAX) {
                b->bad = true;
                break;
            }
            if (c == '\r') {
                b->state = CH_TRAILER_LF;
            }
            i++;
            break;
        case CH_TRAILER_LF:
            if (c != '\n') {
                b->bad = true;
                break;
            }
            b->state = CH_TRAILER_START;
            i++;
            break;
        case CH_END_LF:
            if (c != '\n') {
                b->bad = true;
                break;
            }
            b->done = true;
            i++;
            break;
        default:
            b->bad = true;
            break;
        }
    }
    return i;
}
