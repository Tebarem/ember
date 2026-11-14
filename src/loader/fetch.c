/*
 * Fetch core (W7c). See fetch.h: Headers, the body, the response record and the fetch operation
 * over a transport. C only.
 */
#include "loader/fetch.h"

#include "loader/loader.h"

#include <string.h>

#define FETCH_MAX_REDIRECTS 20u

/* ------------------------------------------------------------------ small helpers */

static bool is_token_char(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}

static bool is_http_ws(unsigned char c)
{
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

/* ASCII-lowercase copy of s into out (appends). */
static void append_lower(embi_sb *out, embi_str s)
{
    for (uint32_t i = 0; i < s.len; i++) {
        char c = s.ptr[i];
        embi_sb_append_char(out, (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
    }
}

static embi_str sb_str(const embi_sb *sb)
{
    return (embi_str){sb->data, sb->len};
}

/* ------------------------------------------------------------------ Headers */

void embi_headers_free(embi_headers *h)
{
    for (uint32_t i = 0; i < h->list.len; i++) {
        embi_sb_free(&h->list.data[i].name);
        embi_sb_free(&h->list.data[i].value);
    }
    embi_free(h->list.data, (size_t)h->list.cap * sizeof h->list.data[0]);
    *h = (embi_headers){0};
}

bool embi_header_name_ok(embi_str name)
{
    if (name.len == 0) {
        return false;
    }
    for (uint32_t i = 0; i < name.len; i++) {
        if (!is_token_char((unsigned char)name.ptr[i])) {
            return false;
        }
    }
    return true;
}

bool embi_header_value_ok(embi_str value)
{
    embi_str t = embi_header_trim(value);
    for (uint32_t i = 0; i < t.len; i++) {
        if (t.ptr[i] == '\0' || t.ptr[i] == '\r' || t.ptr[i] == '\n') {
            return false;
        }
    }
    return true;
}

embi_str embi_header_trim(embi_str value)
{
    uint32_t a = 0;
    uint32_t b = value.len;
    while (a < b && is_http_ws((unsigned char)value.ptr[a])) {
        a++;
    }
    while (b > a && is_http_ws((unsigned char)value.ptr[b - 1])) {
        b--;
    }
    if (a == b) {
        return (embi_str){nullptr, 0};
    }
    return (embi_str){value.ptr + a, b - a};
}

static bool ieq_list(embi_str name, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        embi_str s = {list[i], (uint32_t)strlen(list[i])};
        if (embi_str_ieq(name, s)) {
            return true;
        }
    }
    return false;
}

bool embi_header_forbidden_request(embi_str name)
{
    static const char *const names[] = {
        "accept-charset", "accept-encoding", "access-control-request-headers", "access-control-request-method",
        "connection", "content-length", "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
        "origin", "referer", "set-cookie", "te", "trailer", "transfer-encoding", "upgrade", "via",
    };
    if (ieq_list(name, names, sizeof names / sizeof names[0])) {
        return true;
    }
    if (name.len >= 6 && embi_str_ieq((embi_str){name.ptr, 6}, EMBI_STR("proxy-"))) {
        return true;
    }
    return name.len >= 4 && embi_str_ieq((embi_str){name.ptr, 4}, EMBI_STR("sec-"));
}

bool embi_header_forbidden_response(embi_str name)
{
    static const char *const names[] = {"set-cookie", "set-cookie2"};
    return ieq_list(name, names, sizeof names / sizeof names[0]);
}

static bool guard_hides_request(const embi_headers *h, embi_str name)
{
    return (h->guard == EMBI_GUARD_REQUEST || h->guard == EMBI_GUARD_REQUEST_NO_CORS) &&
           embi_header_forbidden_request(name);
}

static bool guard_hides_response(const embi_headers *h, embi_str name)
{
    return h->guard == EMBI_GUARD_RESPONSE && embi_header_forbidden_response(name);
}

/* Adds an entry; name is lowercased, value trimmed. Returns false on allocation failure (fatal). */
static void push_entry(embi_headers *h, embi_str name, embi_str value)
{
    embi_vec_reserve(&h->list, h->list.len + 1u);
    embi_kv *e = &h->list.data[h->list.len++];
    *e = (embi_kv){{nullptr, 0, 0}, {nullptr, 0, 0}};
    append_lower(&e->name, name);
    embi_sb_append(&e->value, value);
}

void embi_headers_add_raw(embi_headers *h, embi_str name, embi_str value)
{
    push_entry(h, name, embi_header_trim(value));
}

/* Unsafe bytes of a CORS-safelisted header value (Fetch 2.2.2). */
static bool unsafe_byte(unsigned char c)
{
    return c <= 0x08 || (c >= 0x0A && c <= 0x1F) || c == 0x22 || c == 0x28 || c == 0x29 || c == 0x3A || c == 0x3C ||
           c == 0x3E || c == 0x3F || c == 0x40 || c == 0x5B || c == 0x5C || c == 0x5D || c == 0x7B || c == 0x7D ||
           c == 0x7F;
}

/* The bytes allowed in accept-language and content-language values. */
static bool language_byte(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '*' ||
           c == ',' || c == '-' || c == '.' || c == ';' || c == '=';
}

/* Whether a combined value is a CORS-safelisted value of the named request header (Fetch 2.2.2): at most 128
 * bytes, and the name's own rule. Names other than the four safelisted ones are never safelisted. */
static bool safelisted_request_value(embi_str name, embi_str value)
{
    /* Header values are stored as UTF-8 of code points 0-255; the 128 limit counts byte-string units, so a
     * continuation byte does not count. */
    uint32_t units = 0;
    for (uint32_t i = 0; i < value.len; i++) {
        if (((unsigned char)value.ptr[i] & 0xC0) != 0x80) {
            units++;
        }
    }
    if (units > 128) {
        return false;
    }
    if (embi_str_ieq(name, EMBI_STR("accept-language")) || embi_str_ieq(name, EMBI_STR("content-language"))) {
        for (uint32_t i = 0; i < value.len; i++) {
            if (!language_byte((unsigned char)value.ptr[i])) {
                return false;
            }
        }
        return true;
    }
    for (uint32_t i = 0; i < value.len; i++) {
        if (unsafe_byte((unsigned char)value.ptr[i])) {
            return false;
        }
    }
    if (embi_str_ieq(name, EMBI_STR("accept"))) {
        return true;
    }
    if (!embi_str_ieq(name, EMBI_STR("content-type"))) {
        return false;
    }
    /* content-type: the essence must be one of the three form and text types. */
    embi_mime m = {0};
    bool ok = false;
    if (embi_mime_parse(value, &m)) {
        embi_sb essence = {nullptr, 0, 0};
        embi_mime_essence(&m, &essence);
        ok = embi_str_ieq(sb_str(&essence), EMBI_STR("application/x-www-form-urlencoded")) ||
             embi_str_ieq(sb_str(&essence), EMBI_STR("multipart/form-data")) ||
             embi_str_ieq(sb_str(&essence), EMBI_STR("text/plain"));
        embi_sb_free(&essence);
    }
    embi_mime_free(&m);
    return ok;
}

/* Whether a request-no-cors guard lets name: value through (Fetch 5.2, "request-no-cors"): the name must be
 * one of the CORS-safelisted request-headers, and the value that would result (the combined value for append)
 * must be a safelisted value of it. */
static bool no_cors_allows(const embi_headers *h, embi_str name, embi_str value, bool append)
{
    if (h->guard != EMBI_GUARD_REQUEST_NO_CORS) {
        return true;
    }
    embi_str t = embi_header_trim(value);
    embi_sb comb = {nullptr, 0, 0};
    if (append) {
        bool any = false;
        for (uint32_t i = 0; i < h->list.len; i++) {
            if (embi_str_ieq(sb_str(&h->list.data[i].name), name)) {
                if (any) {
                    embi_sb_append(&comb, EMBI_STR(", "));
                }
                embi_sb_append(&comb, sb_str(&h->list.data[i].value));
                any = true;
            }
        }
        if (any) {
            embi_sb_append(&comb, EMBI_STR(", "));
        }
    }
    embi_sb_append(&comb, t);
    bool ok = safelisted_request_value(name, sb_str(&comb));
    embi_sb_free(&comb);
    return ok;
}

static embi_hdr_status check_change(const embi_headers *h, embi_str name, embi_str value)
{
    if (h->guard == EMBI_GUARD_IMMUTABLE) {
        return EMBI_HDR_IMMUTABLE;
    }
    if (!embi_header_name_ok(name) || !embi_header_value_ok(value)) {
        return EMBI_HDR_INVALID;
    }
    return EMBI_HDR_OK;
}

embi_hdr_status embi_headers_append(embi_headers *h, embi_str name, embi_str value)
{
    embi_hdr_status st = check_change(h, name, value);
    if (st != EMBI_HDR_OK || guard_hides_request(h, name) || guard_hides_response(h, name)) {
        return st;
    }
    if (!no_cors_allows(h, name, value, true)) {
        return EMBI_HDR_OK;
    }
    push_entry(h, name, embi_header_trim(value));
    return EMBI_HDR_OK;
}

/* Index of the first entry named name, or UINT32_MAX. */
static uint32_t find_first(const embi_headers *h, embi_str name)
{
    for (uint32_t i = 0; i < h->list.len; i++) {
        if (embi_str_eq(sb_str(&h->list.data[i].name), name)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static void remove_at(embi_headers *h, uint32_t i)
{
    embi_sb_free(&h->list.data[i].name);
    embi_sb_free(&h->list.data[i].value);
    for (uint32_t k = i + 1; k < h->list.len; k++) {
        h->list.data[k - 1] = h->list.data[k];
    }
    h->list.len--;
}

static embi_str lower_name(embi_str name, embi_sb *tmp)
{
    append_lower(tmp, name);
    return sb_str(tmp);
}

embi_hdr_status embi_headers_set(embi_headers *h, embi_str name, embi_str value)
{
    embi_hdr_status st = check_change(h, name, value);
    if (st != EMBI_HDR_OK || guard_hides_request(h, name) || guard_hides_response(h, name)) {
        return st;
    }
    if (!no_cors_allows(h, name, value, false)) {
        return EMBI_HDR_OK;
    }
    embi_sb tmp = {nullptr, 0, 0};
    embi_str ln = lower_name(name, &tmp);
    embi_str v = embi_header_trim(value);
    uint32_t first = find_first(h, ln);
    if (first == UINT32_MAX) {
        push_entry(h, name, v);
    } else {
        /* The first entry keeps its place and takes the value; the others go. */
        embi_sb_clear(&h->list.data[first].value);
        embi_sb_append(&h->list.data[first].value, v);
        for (uint32_t i = first + 1; i < h->list.len;) {
            if (embi_str_eq(sb_str(&h->list.data[i].name), ln)) {
                remove_at(h, i);
            } else {
                i++;
            }
        }
    }
    embi_sb_free(&tmp);
    return EMBI_HDR_OK;
}

embi_hdr_status embi_headers_delete(embi_headers *h, embi_str name)
{
    if (h->guard == EMBI_GUARD_IMMUTABLE) {
        return EMBI_HDR_IMMUTABLE;
    }
    if (!embi_header_name_ok(name)) {
        return EMBI_HDR_INVALID;
    }
    if (guard_hides_request(h, name) || guard_hides_response(h, name)) {
        return EMBI_HDR_OK;
    }
    embi_sb tmp = {nullptr, 0, 0};
    embi_str ln = lower_name(name, &tmp);
    for (uint32_t i = 0; i < h->list.len;) {
        if (embi_str_eq(sb_str(&h->list.data[i].name), ln)) {
            remove_at(h, i);
        } else {
            i++;
        }
    }
    embi_sb_free(&tmp);
    return EMBI_HDR_OK;
}

bool embi_headers_has(const embi_headers *h, embi_str name)
{
    embi_sb tmp = {nullptr, 0, 0};
    embi_str ln = lower_name(name, &tmp);
    bool found = find_first(h, ln) != UINT32_MAX;
    embi_sb_free(&tmp);
    return found;
}

embi_str embi_headers_first(const embi_headers *h, embi_str name)
{
    embi_sb tmp = {nullptr, 0, 0};
    embi_str ln = lower_name(name, &tmp);
    uint32_t i = find_first(h, ln);
    embi_str out = {nullptr, 0};
    if (i != UINT32_MAX) {
        out = sb_str(&h->list.data[i].value);
        /* The value is copied out by the caller before the next change, so it stays borrowed from the list. */
    }
    embi_sb_free(&tmp);
    return out;
}

void embi_header_value_to_wire(embi_str value, embi_sb *out)
{
    const char *p = value.ptr;
    const char *end = value.ptr ? value.ptr + value.len : nullptr;
    while (p && p < end) {
        uint32_t cp = embi_utf8_decode(&p, end);
        char b = (char)(cp > 0xFF ? '?' : cp);
        embi_sb_append(out, (embi_str){&b, 1});
    }
}

void embi_header_value_from_wire(embi_str value, embi_sb *out)
{
    for (uint32_t i = 0; i < value.len; i++) {
        embi_sb_append_cp(out, (unsigned char)value.ptr[i]);
    }
}

bool embi_headers_get(const embi_headers *h, embi_str name, embi_sb *out)
{
    embi_sb tmp = {nullptr, 0, 0};
    embi_str ln = lower_name(name, &tmp);
    bool found = false;
    for (uint32_t i = 0; i < h->list.len; i++) {
        if (embi_str_eq(sb_str(&h->list.data[i].name), ln)) {
            if (found) {
                embi_sb_append(out, EMBI_STR(", "));
            }
            embi_sb_append(out, sb_str(&h->list.data[i].value));
            found = true;
        }
    }
    embi_sb_free(&tmp);
    return found;
}

/* Insertion sort by name (byte order). Stable, and lists are short. */
static void sort_list(embi_kv *v, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        embi_kv cur = v[i];
        uint32_t j = i;
        while (j > 0 && embi_str_icmp(sb_str(&v[j - 1].name), sb_str(&cur.name)) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = cur;
    }
}

bool embi_headers_sorted(const embi_headers *h, embi_headers *out)
{
    *out = (embi_headers){0};
    out->guard = h->guard;
    if (h->list.len == 0) {
        return true;
    }
    embi_kv *tmp = embi_alloc((size_t)h->list.len * sizeof *tmp);
    for (uint32_t i = 0; i < h->list.len; i++) {
        tmp[i] = (embi_kv){{nullptr, 0, 0}, {nullptr, 0, 0}};
        embi_sb_append(&tmp[i].name, sb_str(&h->list.data[i].name));
        embi_sb_append(&tmp[i].value, sb_str(&h->list.data[i].value));
    }
    sort_list(tmp, h->list.len);
    /* Combine the values of equal names, except set-cookie, which keeps one entry per value. */
    for (uint32_t i = 0; i < h->list.len; i++) {
        /* Compared with the last entry of out: tmp[i - 1] may already have been merged and cleared. */
        if (out->list.len > 0 && embi_str_eq(sb_str(&tmp[i].name), sb_str(&out->list.data[out->list.len - 1].name)) &&
            !embi_str_eq(sb_str(&tmp[i].name), EMBI_STR("set-cookie"))) {
            /* Merged into the last entry of out. */
            embi_kv *last = &out->list.data[out->list.len - 1];
            embi_sb_append(&last->value, EMBI_STR(", "));
            embi_sb_append(&last->value, sb_str(&tmp[i].value));
            embi_sb_free(&tmp[i].name);
            embi_sb_free(&tmp[i].value);
            continue;
        }
        embi_vec_reserve(&out->list, out->list.len + 1u);
        out->list.data[out->list.len++] = tmp[i];
    }
    /* The entries moved into out are not freed here; the stale copies are cleared. */
    embi_free(tmp, (size_t)h->list.len * sizeof *tmp);
    return true;
}

bool embi_headers_copy_filtered(const embi_headers *src, embi_headers *dst, bool hide_set_cookie)
{
    *dst = (embi_headers){0};
    for (uint32_t i = 0; i < src->list.len; i++) {
        embi_str n = sb_str(&src->list.data[i].name);
        if (hide_set_cookie && embi_header_forbidden_response(n)) {
            continue;
        }
        push_entry(dst, n, sb_str(&src->list.data[i].value));
    }
    return true;
}

/* ------------------------------------------------------------------ methods */

bool embi_method_normalize(embi_str method, embi_sb *out)
{
    if (method.len == 0) {
        return false;
    }
    for (uint32_t i = 0; i < method.len; i++) {
        if (!is_token_char((unsigned char)method.ptr[i])) {
            return false;
        }
    }
    if (embi_str_ieq(method, EMBI_STR("CONNECT")) || embi_str_ieq(method, EMBI_STR("TRACE")) ||
        embi_str_ieq(method, EMBI_STR("TRACK"))) {
        return false;
    }
    static const char *const standard[] = {"DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"};
    for (size_t i = 0; i < sizeof standard / sizeof standard[0]; i++) {
        embi_str s = {standard[i], (uint32_t)strlen(standard[i])};
        if (embi_str_ieq(method, s)) {
            embi_sb_append(out, s);
            return true;
        }
    }
    embi_sb_append(out, method);
    return true;
}

/* ------------------------------------------------------------------ body and response */

void embi_response_free(embi_response *r)
{
    embi_sb_free(&r->status_text);
    embi_headers_free(&r->headers);
    embi_sb_free(&r->body.bytes);
    embi_sb_free(&r->url);
    *r = (embi_response){0};
}

bool embi_status_ok(uint32_t status)
{
    return status >= 200 && status <= 599;
}

bool embi_status_null_body(uint32_t status)
{
    return status == 101 || status == 103 || status == 204 || status == 205 || status == 304;
}

bool embi_status_redirect(uint32_t status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool embi_status_text_ok(embi_str text)
{
    for (uint32_t i = 0; i < text.len; i++) {
        unsigned char c = (unsigned char)text.ptr[i];
        bool ok = c == '\t' || c == ' ' || (c >= 0x21 && c <= 0x7e) || c >= 0x80;
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool embi_body_consume(embi_body *b, embi_sb *out)
{
    if (!b->present) {
        embi_sb_clear(out);
        return true;
    }
    if (b->used) {
        return false;
    }
    embi_sb_free(out);
    *out = b->bytes;
    b->bytes = (embi_sb){nullptr, 0, 0};
    b->used = true;
    return true;
}

void embi_response_error(embi_response *out)
{
    embi_response_free(out);
    out->type = EMBI_RESP_ERROR;
    out->status = 0;
}

/* ------------------------------------------------------------------ fetch operation */

typedef enum op_state {
    OP_PENDING,
    OP_COMPLETE, /* done was called (ok or failed) */
    OP_ABORTED,
} op_state;

struct embi_fetch_op {
    const embi_fetch_transport *t;
    embi_fetch_done_fn done;
    void *user;
    embi_sb method;
    embi_url url;          /* the current URL of the chain */
    embi_headers headers;  /* the request headers, copied */
    embi_sb body;          /* the request body, copied */
    bool has_body;
    embi_redirect redirect;
    embi_fetch_creds creds;
    embi_sb origin;        /* the initiator's origin (may be empty) */
    uint32_t hops;
    uint32_t hop_seq;      /* counts the hops started, so a nested start does not overwrite the handle of a later hop */
    void *handle;          /* the transport's token of the hop in flight */
    bool redirected;
    bool in_hop;           /* the transport owns a hop (between start and its hop_done; a streamed body keeps it until its end) */
    op_state state;
    bool failed;
    bool streaming;        /* the head is done and the body is still arriving through the sink */
    bool has_sink;
    embi_fetch_sink sink;
    embi_response resp;
    embi_sb err;
};

static void op_fail(embi_fetch_op *op, const char *msg)
{
    op->state = OP_COMPLETE;
    op->failed = true;
    embi_response_error(&op->resp);
    embi_sb_append(&op->err, (embi_str){msg, (uint32_t)strlen(msg)});
    op->done(op->user, op, false);
}

/* Starts the hop of op->url. Returns false when the transport could not start it (the caller fails). */
static bool op_start_hop(embi_fetch_op *op)
{
    uint32_t n = op->headers.list.len;
    embi_hdr_view *views = n > 0 ? embi_alloc((size_t)n * sizeof *views) : nullptr;
    for (uint32_t i = 0; i < n; i++) {
        views[i] = (embi_hdr_view){sb_str(&op->headers.list.data[i].name), sb_str(&op->headers.list.data[i].value)};
    }
    embi_str method = sb_str(&op->method);
    embi_str url = {op->url.href, op->url.href_len};
    op->in_hop = true;
    const uint8_t *body = op->has_body ? (const uint8_t *)op->body.data : nullptr;
    uint32_t seq = ++op->hop_seq;
    void *handle = nullptr;
    bool ok = op->t->start(op->t->user, op, &handle, method, url, views, n, body,
                           op->has_body ? op->body.len : 0);
    if (n > 0) {
        embi_free(views, (size_t)n * sizeof *views);
    }
    if (!ok) {
        op->in_hop = false;
    } else if (op->in_hop && op->hop_seq == seq) {
        /* Still this hop in flight: a hop completed inside start (and a nested hop after it) leaves the field alone. */
        op->handle = handle;
    }
    return ok;
}

/* The response of the last hop, final: copy it into op->resp and report. */
static void op_finish(embi_fetch_op *op, const embi_fetch_hop *hop)
{
    /* A status outside 200..599 from a handler is a network error (Fetch 4.1, a response status is 200 to 599 here). */
    if (!embi_status_ok(hop->status)) {
        op_fail(op, "invalid response status");
        return;
    }
    op->state = OP_COMPLETE;
    op->failed = false;
    embi_response *r = &op->resp;
    embi_response_free(r);
    r->type = hop->cors ? EMBI_RESP_CORS : EMBI_RESP_BASIC;
    r->status = hop->status;
    if (hop->status_text.len > 0) {
        embi_sb_append(&r->status_text, hop->status_text);
    }
    r->redirected = op->redirected || hop->redirected;
    if (hop->url.len > 0) {
        embi_sb_append(&r->url, hop->url);
    } else {
        embi_sb_append(&r->url, (embi_str){op->url.href, op->url.href_len});
    }
    /* The hop's headers, with the set-cookie entries hidden (basic response). */
    for (uint32_t i = 0; i < hop->nheaders; i++) {
        embi_str n = hop->headers[i].name;
        if (embi_header_forbidden_response(n)) {
            continue;
        }
        embi_headers_add_raw(&r->headers, n, hop->headers[i].value);
    }
    if (hop->mime.ptr != nullptr && hop->mime.len > 0 && !embi_headers_has(&r->headers, EMBI_STR("content-type"))) {
        embi_headers_add_raw(&r->headers, EMBI_STR("content-type"), hop->mime);
    }
    r->body.present = !embi_status_null_body(hop->status);
    if (r->body.present && hop->body != nullptr && hop->body_len > 0) {
        embi_sb_append(&r->body.bytes, (embi_str){(const char *)hop->body, (uint32_t)hop->body_len});
    }
    op->done(op->user, op, true);
}

/* Redirect handling (Fetch 4.4 "HTTP-redirect fetch"): returns true when the chain went on (a new hop started or
 * the operation failed inside), false when the response is final. */
static bool op_redirect(embi_fetch_op *op, const embi_fetch_hop *hop)
{
    if (!embi_status_redirect(hop->status)) {
        return false;
    }
    embi_str loc = {nullptr, 0};
    bool have_loc = false;
    for (uint32_t i = 0; i < hop->nheaders; i++) {
        if (embi_str_ieq(hop->headers[i].name, EMBI_STR("location"))) {
            loc = embi_header_trim(hop->headers[i].value);
            have_loc = true;
            break;
        }
    }
    if (!have_loc) {
        return false; /* no Location header: the response is final */
    }
    if (op->redirect == EMBI_REDIRECT_ERROR) {
        op_fail(op, "redirect mode is error");
        return true;
    }
    if (op->redirect == EMBI_REDIRECT_MANUAL) {
        /* An opaque redirect: status 0, no body, no headers. */
        op->state = OP_COMPLETE;
        op->failed = false;
        embi_response_free(&op->resp);
        op->resp.type = EMBI_RESP_OPAQUEREDIRECT;
        op->resp.status = 0;
        op->resp.body.present = false;
        embi_sb_append(&op->resp.url, (embi_str){op->url.href, op->url.href_len});
        op->done(op->user, op, true);
        return true;
    }
    if (op->hops >= FETCH_MAX_REDIRECTS) {
        op_fail(op, "too many redirects");
        return true;
    }
    embi_url next = {0};
    if (loc.len == 0 || !embi_url_parse(loc, &op->url, &next)) { /* present but empty or unparsable: a network error */
        op_fail(op, "invalid redirect location");
        return true;
    }
    op->hops++;
    /* Fetch 4.4 step 13: a redirect to another origin drops the Authorization header. An opaque origin counts as
     * different. */
    {
        embi_sb from = {nullptr, 0, 0}, to = {nullptr, 0, 0};
        embi_url_origin(&op->url, &from);
        embi_url_origin(&next, &to);
        bool same = from.len > 0 && !embi_str_eq(sb_str(&from), EMBI_STR("null")) && embi_str_eq(sb_str(&from), sb_str(&to));
        embi_sb_free(&from);
        embi_sb_free(&to);
        if (!same) {
            (void)embi_headers_delete(&op->headers, EMBI_STR("authorization"));
        }
    }
    /* Fetch 4.4 step 11: 303 changes a non-GET/HEAD method to GET, and 301 and 302 change POST to GET. The
     * body and the body headers go with it. */
    bool to_get = (hop->status == 303 && !embi_str_eq(sb_str(&op->method), EMBI_STR("GET")) &&
                   !embi_str_eq(sb_str(&op->method), EMBI_STR("HEAD"))) ||
                  ((hop->status == 301 || hop->status == 302) && embi_str_eq(sb_str(&op->method), EMBI_STR("POST")));
    if (to_get) {
        embi_sb_clear(&op->method);
        embi_sb_append(&op->method, EMBI_STR("GET"));
        op->has_body = false;
        embi_sb_clear(&op->body);
        static const char *const body_headers[] = {"content-encoding", "content-language", "content-location",
                                                   "content-type"};
        for (size_t k = 0; k < sizeof body_headers / sizeof body_headers[0]; k++) {
            embi_str n = {body_headers[k], (uint32_t)strlen(body_headers[k])};
            (void)embi_headers_delete(&op->headers, n);
        }
    }
    embi_url_free(&op->url);
    op->url = next;
    op->redirected = true;
    if (!op_start_hop(op)) {
        op_fail(op, "no handler for the redirect");
    }
    return true;
}

embi_fetch_op *embi_fetch_op_new(const embi_fetch_transport *t, const embi_fetch_init *in, embi_fetch_done_fn done,
                                 void *user)
{
    embi_fetch_op *op = embi_alloc(sizeof *op);
    *op = (embi_fetch_op){0};
    op->t = t;
    op->done = done;
    op->user = user;
    op->redirect = in->redirect;
    op->creds = in->creds;
    embi_sb_append(&op->origin, in->initiator_origin);
    if (in->sink != nullptr) {
        op->has_sink = true;
        op->sink = *in->sink;
    }
    op->state = OP_PENDING;
    embi_sb_append(&op->method, in->method);
    if (!embi_url_parse(in->url, nullptr, &op->url)) {
        embi_sb_free(&op->method);
        embi_free(op, sizeof *op);
        return nullptr;
    }
    op->headers.guard = EMBI_GUARD_NONE;
    for (uint32_t i = 0; i < in->nheaders; i++) {
        embi_headers_add_raw(&op->headers, in->headers[i].name, in->headers[i].value);
    }
    if (in->body != nullptr) {
        op->has_body = true;
        if (in->body_len > 0) {
            embi_sb_append(&op->body, (embi_str){(const char *)in->body, (uint32_t)in->body_len});
        }
    }
    if (!op_start_hop(op)) {
        op_fail(op, "no handler for the request");
    }
    return op;
}

void embi_fetch_hop_done(embi_fetch_op *op, const embi_fetch_hop *hop)
{
    if (op->state != OP_PENDING || !op->in_hop) {
        return; /* aborted or already completed: the result is dropped */
    }
    op->in_hop = false;
    if (!hop->followed && op_redirect(op, hop)) {
        return;
    }
    op_finish(op, hop);
}

void embi_fetch_hop_stream_begin(embi_fetch_op *op, const embi_fetch_hop *head)
{
    if (op->state != OP_PENDING || !op->in_hop) {
        return;
    }
    embi_fetch_hop h = *head;
    h.body = nullptr;
    h.body_len = 0;
    op->streaming = true; /* set before done: the completion may ask for it */
    op_finish(op, &h);
    if (op->state != OP_COMPLETE || op->failed) {
        /* A bad status: the operation failed inside op_finish; the transport's hop is no longer wanted. */
        op->streaming = false;
        op->in_hop = false;
        if (op->t->abort != nullptr) {
            op->t->abort(op->t->user, op->handle);
        }
    }
}

void embi_fetch_hop_stream_chunk(embi_fetch_op *op, const uint8_t *data, size_t len)
{
    if (!op->streaming || !op->in_hop || len == 0 || !op->has_sink) {
        return;
    }
    op->sink.chunk(op->sink.user, data, len);
}

void embi_fetch_hop_stream_end(embi_fetch_op *op)
{
    if (!op->streaming || !op->in_hop) {
        return;
    }
    op->in_hop = false;
    op->streaming = false;
    if (op->has_sink) {
        op->sink.end(op->sink.user);
    }
}

void embi_fetch_hop_fail(embi_fetch_op *op, embi_str message)
{
    if (!op->in_hop) {
        return;
    }
    op->in_hop = false;
    if (op->state == OP_PENDING) {
        embi_sb_clear(&op->err);
        embi_sb_append(&op->err, message);
        op->state = OP_COMPLETE;
        op->failed = true;
        embi_response_error(&op->resp);
        op->done(op->user, op, false);
        return;
    }
    if (op->streaming) {
        op->streaming = false;
        if (op->has_sink) {
            op->sink.error(op->sink.user, message);
        }
    }
}

bool embi_fetch_op_streaming(const embi_fetch_op *op)
{
    return op->streaming;
}

void embi_fetch_op_options(const embi_fetch_op *op, embi_fetch_options *out)
{
    out->redirect = op->redirect;
    out->creds = op->creds;
    out->origin = sb_str(&op->origin);
}

void embi_fetch_op_abort(embi_fetch_op *op)
{
    if (op->state == OP_PENDING) {
        op->state = OP_ABORTED;
    } else if (!op->streaming) {
        return; /* completed (or failed): nothing is in flight */
    }
    op->streaming = false;
    if (op->in_hop) {
        op->in_hop = false;
        if (op->t->abort != nullptr) {
            op->t->abort(op->t->user, op->handle);
        }
    }
}

void embi_fetch_op_hold(embi_fetch_op *op, bool on)
{
    if (op->in_hop && op->t->hold != nullptr) {
        op->t->hold(op->t->user, op->handle, on);
    }
}

bool embi_fetch_op_pending(const embi_fetch_op *op)
{
    return op->state == OP_PENDING;
}

void embi_fetch_op_take(embi_fetch_op *op, embi_response *out)
{
    *out = op->resp;
    op->resp = (embi_response){0};
}

embi_str embi_fetch_op_error(const embi_fetch_op *op)
{
    return sb_str(&op->err);
}

void embi_fetch_op_free(embi_fetch_op *op)
{
    if (op == nullptr) {
        return;
    }
    embi_fetch_op_abort(op);
    embi_sb_free(&op->method);
    embi_sb_free(&op->origin);
    embi_url_free(&op->url);
    embi_headers_free(&op->headers);
    embi_sb_free(&op->body);
    embi_response_free(&op->resp);
    embi_sb_free(&op->err);
    embi_free(op, sizeof *op);
}
