/*
 * The host allowlist (N1): patterns "host", "host:port", "*.domain[:port]" and "*", matched against the URL parser's
 * normalized host and the request's effective port. Checked before any DNS lookup and for every redirect target.
 */
#include "net_int.h"

#include <ctype.h>

/* Normalizes a host (or IPv6 literal in brackets) the way URLs do, through the URL parser. Appends to out. */
static bool normalize_host(const char *host, size_t len, embi_sb *out)
{
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c <= 0x20 || c == '/' || c == 0x5C || c == '?' || c == '#' || c == '@' || c == '*' || c == '%') {
            return false;
        }
    }
    embi_sb tmp = {0};
    embi_sb_append(&tmp, EMBI_STR("http://"));
    embi_sb_append(&tmp, (embi_str){host, (uint32_t)len});
    embi_sb_append_char(&tmp, '/');
    embi_url u = {0};
    bool ok = embi_url_parse((embi_str){tmp.data, tmp.len}, nullptr, &u);
    embi_sb_free(&tmp);
    if (!ok || !u.has_host || u.host_len == 0 || u.port != -1 || u.user_len != 0 || u.pass_len != 0) {
        embi_url_free(&u);
        return false;
    }
    embi_sb_append(out, (embi_str){u.href + u.host_off, u.host_len});
    embi_url_free(&u);
    return true;
}

bool embi_net_pattern_parse(const char *s, embi_net_pattern *out)
{
    *out = (embi_net_pattern){0};
    if (s == nullptr) {
        return false;
    }
    size_t n = strlen(s);
    if (n == 1 && s[0] == '*') {
        return true;
    }
    const char *host = s;
    size_t hlen = n;
    bool sub = false;
    if (n >= 2 && s[0] == '*' && s[1] == '.') {
        sub = true;
        host = s + 2;
        hlen = n - 2;
    }
    int32_t port = 0;
    /* The port is after the last ':' that is outside brackets. */
    const char *colon = nullptr;
    bool in_br = false;
    for (size_t i = 0; i < hlen; i++) {
        if (host[i] == '[') {
            in_br = true;
        } else if (host[i] == ']') {
            in_br = false;
        } else if (host[i] == ':' && !in_br) {
            colon = host + i;
        }
    }
    if (colon != nullptr) {
        size_t plen = hlen - (size_t)(colon + 1 - host);
        if (plen == 0 || plen > 5) {
            return false;
        }
        int32_t p = 0;
        for (size_t i = 0; i < plen; i++) {
            char c = colon[1 + i];
            if (c < '0' || c > '9') {
                return false;
            }
            p = p * 10 + (c - '0');
        }
        if (p < 1 || p > 65535) {
            return false;
        }
        port = p;
        hlen = (size_t)(colon - host);
    }
    if (sub && host[0] == '[') {
        return false;
    }
    embi_sb norm = {0};
    if (!normalize_host(host, hlen, &norm)) {
        embi_sb_free(&norm);
        return false;
    }
    if (sub && (norm.data[0] == '[' || norm.data[norm.len - 1] == '.')) {
        embi_sb_free(&norm);
        return false;
    }
    out->host = embi_net_strndup(norm.data, norm.len);
    out->sub = sub;
    out->port = port;
    embi_sb_free(&norm);
    return true;
}

bool embi_net_pattern_match(const embi_net_pattern *p, embi_str host, int port)
{
    if (p->port != 0 && p->port != port) {
        return false;
    }
    if (p->host == nullptr) {
        return true;
    }
    size_t plen = strlen(p->host);
    if (!p->sub) {
        return host.len == plen && memcmp(host.ptr, p->host, plen) == 0;
    }
    /* *.example.com: at least one label before ".example.com". */
    if (host.len <= plen + 1) {
        return false;
    }
    size_t off = host.len - plen;
    return host.ptr[off - 1] == '.' && memcmp(host.ptr + off, p->host, plen) == 0;
}

void embi_net_pattern_free(embi_net_pattern *p)
{
    embi_net_mfree(p->host);
    *p = (embi_net_pattern){0};
}
