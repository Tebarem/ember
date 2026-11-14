/*
 * The cookie jar (N1): RFC 6265bis in memory. Domain, Path, Expires and Max-Age, Secure, HttpOnly; SameSite is
 * recorded but not applied (there is no site context). There is no public suffix list: a Domain attribute with no dot
 * is accepted only when it equals the request host, but "co.uk" style attributes are accepted (a known gap: the
 * allowlist limits which hosts can set cookies at all, and the embedder allowlists hosts it trusts).
 * "Leave Secure cookies alone" (6265bis 5.7): an insecure origin cannot set or overwrite a cookie whose name matches an
 * existing Secure cookie with an overlapping domain and path. Limits: COOKIE_MAX_PER_DOMAIN per domain and
 * COOKIE_MAX_TOTAL overall; the oldest cookie (within the domain, then overall) is evicted.
 */
#include "net_int.h"

#define COOKIE_MAX_TOTAL 3000u
#define COOKIE_MAX_PER_DOMAIN 180u
/* Max-Age is clamped here (seconds, about 31,000 years) so that the arithmetic in milliseconds cannot overflow. */
#define COOKIE_MAX_AGE_S INT64_C(1000000000000)
#define COOKIE_MAX_NV 4096u
#define COOKIE_MAX_ATTR 1024u

static bool is_ws(char c)
{
    return c == ' ' || c == '\t';
}

static embi_str trim_ws(embi_str s)
{
    while (s.len > 0 && is_ws(s.ptr[0])) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && is_ws(s.ptr[s.len - 1])) {
        s.len--;
    }
    return s;
}

static void free_cookie_fields(embi_cookie *c)
{
    embi_net_mfree(c->name);
    embi_net_mfree(c->value);
    embi_net_mfree(c->domain);
    embi_net_mfree(c->path);
}

void embi_cookiejar_free(embi_cookiejar *j)
{
    for (uint32_t i = 0; i < j->n; i++) {
        free_cookie_fields(&j->v[i]);
    }
    embi_net_mfree(j->v);
    *j = (embi_cookiejar){0};
}

uint32_t embi_cookiejar_count(const embi_cookiejar *j)
{
    return j->n;
}

static void remove_at(embi_cookiejar *j, uint32_t i)
{
    free_cookie_fields(&j->v[i]);
    j->n--;
    if (i < j->n) {
        j->v[i] = j->v[j->n];
    }
}

/* ------------------------------------------------------------------ dates (RFC 6265 5.1.1) */

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool date_delim(char c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x2F) || (c >= 0x3B && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
           (c >= 0x7B && c <= 0x7E);
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Up to max_digits digits at t[*k]; false when there is none. */
static bool take_digits(const char *t, uint32_t tl, uint32_t *k, int max_digits, int64_t *v)
{
    int digits = 0;
    *v = 0;
    while (*k < tl && is_digit(t[*k]) && digits < max_digits) {
        *v = *v * 10 + (t[*k] - '0');
        (*k)++;
        digits++;
    }
    return digits > 0;
}

bool embi_cookie_parse_date(embi_str s, int64_t *ms)
{
    bool have_time = false, have_dom = false, have_month = false, have_year = false;
    int64_t hh = 0, mm = 0, ss = 0, dom = 0, year = 0;
    int month = 0;
    uint32_t i = 0;
    while (i < s.len) {
        while (i < s.len && date_delim(s.ptr[i])) {
            i++;
        }
        uint32_t b = i;
        while (i < s.len && !date_delim(s.ptr[i])) {
            i++;
        }
        if (b == i) {
            continue;
        }
        const char *t = s.ptr + b;
        uint32_t tl = i - b;
        if (!have_time && is_digit(t[0])) {
            uint32_t k = 0;
            int64_t a, bb, c;
            if (take_digits(t, tl, &k, 2, &a) && k < tl && t[k] == ':' && (k++, take_digits(t, tl, &k, 2, &bb)) &&
                k < tl && t[k] == ':' && (k++, take_digits(t, tl, &k, 2, &c)) && (k == tl || !is_digit(t[k]))) {
                have_time = true;
                hh = a;
                mm = bb;
                ss = c;
                continue;
            }
        }
        if (!have_dom && is_digit(t[0])) {
            uint32_t k = 0;
            int64_t v;
            if (take_digits(t, tl, &k, 2, &v) && (k == tl || !is_digit(t[k]))) {
                have_dom = true;
                dom = v;
                continue;
            }
        }
        if (!have_month && tl >= 3) {
            static const char *const months[12] = {"jan", "feb", "mar", "apr", "may", "jun",
                                                   "jul", "aug", "sep", "oct", "nov", "dec"};
            bool found = false;
            for (int m = 0; m < 12; m++) {
                if (SDL_tolower((unsigned char)t[0]) == months[m][0] && SDL_tolower((unsigned char)t[1]) == months[m][1] &&
                    SDL_tolower((unsigned char)t[2]) == months[m][2]) {
                    have_month = true;
                    month = m + 1;
                    found = true;
                    break;
                }
            }
            if (found) {
                continue;
            }
        }
        if (!have_year && is_digit(t[0])) {
            uint32_t k = 0;
            int64_t v;
            if (take_digits(t, tl, &k, 4, &v) && k >= 2 && (k == tl || !is_digit(t[k]))) {
                have_year = true;
                year = v;
                continue;
            }
        }
    }
    if (!(have_time && have_dom && have_month && have_year)) {
        return false;
    }
    if (year >= 70 && year <= 99) {
        year += 1900;
    } else if (year >= 0 && year <= 69) {
        year += 2000;
    }
    if (dom < 1 || dom > 31 || year < 1601 || hh > 23 || mm > 59 || ss > 59) {
        return false;
    }
    int64_t days = days_from_civil(year, month, (int)dom);
    *ms = ((days * 24 + hh) * 60 + mm) * 60000 + ss * 1000;
    return true;
}

/* ------------------------------------------------------------------ matching */

static bool is_ip_literal(const char *host, size_t len)
{
    if (len > 0 && host[0] == '[') {
        return true;
    }
    bool dots = false;
    for (size_t i = 0; i < len; i++) {
        if (host[i] == '.') {
            dots = true;
        } else if (!is_digit(host[i])) {
            return false;
        }
    }
    return dots;
}

static bool domain_match(const char *host, size_t hlen, const char *domain)
{
    size_t dlen = strlen(domain);
    if (hlen == dlen && memcmp(host, domain, dlen) == 0) {
        return true;
    }
    return hlen > dlen && host[hlen - dlen - 1] == '.' && memcmp(host + hlen - dlen, domain, dlen) == 0 &&
           !is_ip_literal(host, hlen);
}

static bool path_match(const char *req_path, size_t rlen, const char *cpath)
{
    size_t clen = strlen(cpath);
    if (rlen == clen && memcmp(req_path, cpath, clen) == 0) {
        return true;
    }
    if (rlen > clen && memcmp(req_path, cpath, clen) == 0) {
        return cpath[clen - 1] == '/' || req_path[clen] == '/';
    }
    return false;
}

static void default_path(embi_str path, embi_sb *out)
{
    if (path.len == 0 || path.ptr[0] != '/') {
        embi_sb_append_char(out, '/');
        return;
    }
    uint32_t last = 0;
    for (uint32_t i = 0; i < path.len; i++) {
        if (path.ptr[i] == '/') {
            last = i;
        }
    }
    if (last == 0) {
        embi_sb_append_char(out, '/');
    } else {
        embi_sb_append(out, (embi_str){path.ptr, last});
    }
}

static embi_str url_path(const embi_url *u)
{
    embi_str p = {u->href + u->path_off, u->path_len};
    if (p.len == 0) {
        return EMBI_STR("/");
    }
    return p;
}

static bool url_https(const embi_url *u)
{
    return u->scheme_len == 5 && memcmp(u->href, "https", 5) == 0;
}

/* ------------------------------------------------------------------ set */

static bool has_ctl(embi_str s)
{
    for (uint32_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.ptr[i];
        if ((c < 0x20 && c != 0x09) || c == 0x7F) {
            return true;
        }
    }
    return false;
}

/* The attributes of one Set-Cookie value. */
typedef struct sc_attrs {
    bool secure, http_only, have_max_age, have_expires, domain_set;
    uint8_t samesite;
    int64_t max_age_ms, expires_ms;
    embi_sb domain;
    embi_sb path;
} sc_attrs;

static void parse_attr(sc_attrs *a, embi_str an, embi_str aval)
{
    if (aval.len > COOKIE_MAX_ATTR) {
        return;
    }
    if (embi_str_ieq(an, EMBI_STR("secure"))) {
        a->secure = true;
    } else if (embi_str_ieq(an, EMBI_STR("httponly"))) {
        a->http_only = true;
    } else if (embi_str_ieq(an, EMBI_STR("max-age"))) {
        uint32_t k = 0;
        bool neg = false;
        if (k < aval.len && aval.ptr[k] == '-') {
            neg = true;
            k++;
        }
        if (k < aval.len) {
            int64_t v = 0;
            bool ok = true;
            for (; k < aval.len; k++) {
                if (!is_digit(aval.ptr[k])) {
                    ok = false;
                    break;
                }
                if (v < COOKIE_MAX_AGE_S) {
                    v = v * 10 + (aval.ptr[k] - '0');
                }
            }
            if (ok) {
                if (v > COOKIE_MAX_AGE_S) {
                    v = COOKIE_MAX_AGE_S;
                }
                a->have_max_age = true;
                a->max_age_ms = (neg || v == 0) ? -1 : v * 1000;
            }
        }
    } else if (embi_str_ieq(an, EMBI_STR("expires"))) {
        int64_t ms;
        if (embi_cookie_parse_date(aval, &ms)) {
            a->have_expires = true;
            a->expires_ms = ms;
        }
    } else if (embi_str_ieq(an, EMBI_STR("domain"))) {
        if (aval.len > 0 && aval.ptr[0] == '.') {
            aval.ptr++;
            aval.len--;
        }
        embi_sb_clear(&a->domain);
        for (uint32_t i = 0; i < aval.len; i++) {
            embi_sb_append_char(&a->domain, (char)SDL_tolower((unsigned char)aval.ptr[i]));
        }
        a->domain_set = a->domain.len > 0;
    } else if (embi_str_ieq(an, EMBI_STR("path"))) {
        embi_sb_clear(&a->path);
        if (aval.len > 0 && aval.ptr[0] == '/') {
            embi_sb_append(&a->path, aval);
        }
    } else if (embi_str_ieq(an, EMBI_STR("samesite"))) {
        a->samesite = embi_str_ieq(aval, EMBI_STR("none"))     ? 1
                      : embi_str_ieq(aval, EMBI_STR("strict")) ? 3
                                                               : 2;
    }
}

bool embi_cookiejar_set(embi_cookiejar *j, const embi_url *url, embi_str sc, bool from_script, int64_t now_ms)
{
    embi_str host = {url->href + url->host_off, url->host_len};
    if (sc.len > COOKIE_MAX_NV + COOKIE_MAX_ATTR * 4 || host.len == 0) {
        return false;
    }
    /* name=value is the first ';' separated part; without '=' the whole part is the value (6265bis). */
    uint32_t semi = 0;
    while (semi < sc.len && sc.ptr[semi] != ';') {
        semi++;
    }
    embi_str nv = trim_ws((embi_str){sc.ptr, semi});
    embi_str name = {nv.ptr, 0};
    embi_str value = nv;
    for (uint32_t i = 0; i < nv.len; i++) {
        if (nv.ptr[i] == '=') {
            name = trim_ws((embi_str){nv.ptr, i});
            value = trim_ws((embi_str){nv.ptr + i + 1, nv.len - i - 1});
            break;
        }
    }
    if ((name.len == 0 && value.len == 0) || name.len + value.len > COOKIE_MAX_NV || has_ctl(name) || has_ctl(value)) {
        return false;
    }

    sc_attrs a = {0};
    uint32_t pos = semi;
    while (pos < sc.len) {
        pos++; /* skip ';' */
        uint32_t end = pos;
        while (end < sc.len && sc.ptr[end] != ';') {
            end++;
        }
        embi_str av = trim_ws((embi_str){sc.ptr + pos, end - pos});
        pos = end;
        embi_str an = av, aval = {av.ptr + av.len, 0};
        for (uint32_t i = 0; i < av.len; i++) {
            if (av.ptr[i] == '=') {
                an = trim_ws((embi_str){av.ptr, i});
                aval = trim_ws((embi_str){av.ptr + i + 1, av.len - i - 1});
                break;
            }
        }
        parse_attr(&a, an, aval);
    }

    bool ok = true;
    bool host_only = true;
    embi_sb domain = {0};
    if (a.domain_set) {
        bool same = a.domain.len == host.len && memcmp(a.domain.data, host.ptr, host.len) == 0;
        /* No public suffix list: a dotless domain is only accepted when it is the host itself, and an IP host only
         * accepts itself. */
        if (domain_match(host.ptr, host.len, embi_sb_cstr(&a.domain)) &&
            (same || (memchr(a.domain.data, '.', a.domain.len) != nullptr && !is_ip_literal(host.ptr, host.len)))) {
            embi_sb_append(&domain, (embi_str){a.domain.data, a.domain.len});
            host_only = false;
        } else {
            ok = false;
        }
    } else {
        for (uint32_t i = 0; i < host.len; i++) {
            embi_sb_append_char(&domain, (char)SDL_tolower((unsigned char)host.ptr[i]));
        }
    }
    bool https = url_https(url);
    if (a.secure && !https) {
        ok = false;
    }
    if (from_script && a.http_only) {
        ok = false;
    }
    if (name.len >= 9 && SDL_strncasecmp(name.ptr, "__Secure-", 9) == 0 && !(a.secure && https)) {
        ok = false;
    }
    if (name.len >= 7 && SDL_strncasecmp(name.ptr, "__Host-", 7) == 0 &&
        !(a.secure && https && !a.domain_set && a.path.len == 1 && a.path.data[0] == '/')) {
        ok = false;
    }

    embi_sb path = {0};
    bool changed = false;
    if (ok) {
        if (a.path.len > 0) {
            embi_sb_append(&path, (embi_str){a.path.data, a.path.len});
        } else {
            default_path(url_path(url), &path);
        }
        int64_t expiry = INT64_MAX;
        if (a.have_max_age) {
            expiry = a.max_age_ms < 0 ? 0 : now_ms + a.max_age_ms;
        } else if (a.have_expires) {
            expiry = a.expires_ms;
        }
        /* An insecure origin cannot shadow a Secure cookie (leave-secure-cookies-alone). */
        if (!a.secure && !https) {
            for (uint32_t i = 0; i < j->n; i++) {
                const embi_cookie *c = &j->v[i];
                if (c->secure && c->expiry_ms > now_ms && strlen(c->name) == name.len && memcmp(c->name, name.ptr, name.len) == 0 &&
                    (domain_match(embi_sb_cstr(&domain), domain.len, c->domain) || domain_match(c->domain, strlen(c->domain), embi_sb_cstr(&domain))) &&
                    path_match(path.data, path.len, c->path)) {
                    ok = false;
                    break;
                }
            }
        }
        /* Replace the cookie with the same name, domain, host-only flag and path (keeping its creation time). */
        int64_t created = now_ms;
        uint64_t seq = ++j->seq;
        for (uint32_t i = 0; ok && i < j->n; i++) {
            embi_cookie *c = &j->v[i];
            if (strlen(c->name) == name.len && memcmp(c->name, name.ptr, name.len) == 0 && c->host_only == host_only &&
                strlen(c->domain) == domain.len && memcmp(c->domain, domain.data, domain.len) == 0 &&
                strlen(c->path) == path.len && memcmp(c->path, path.data, path.len) == 0) {
                if (c->http_only && from_script) {
                    ok = false;
                    break;
                }
                created = c->created_ms;
                seq = c->seq;
                remove_at(j, i);
                changed = true;
                break;
            }
        }
        if (ok && expiry > now_ms) {
            uint32_t in_domain = 0, oldest_in_domain = UINT32_MAX;
            for (uint32_t i = 0; i < j->n; i++) {
                if (strlen(j->v[i].domain) == domain.len && memcmp(j->v[i].domain, domain.data, domain.len) == 0) {
                    in_domain++;
                    if (oldest_in_domain == UINT32_MAX || j->v[i].seq < j->v[oldest_in_domain].seq) {
                        oldest_in_domain = i;
                    }
                }
            }
            if (in_domain >= COOKIE_MAX_PER_DOMAIN) {
                remove_at(j, oldest_in_domain);
            }
            if (j->n >= COOKIE_MAX_TOTAL) {
                uint32_t oldest = 0;
                for (uint32_t i = 1; i < j->n; i++) {
                    if (j->v[i].seq < j->v[oldest].seq) {
                        oldest = i;
                    }
                }
                remove_at(j, oldest);
            }
            if (j->n == j->cap) {
                uint32_t ncap = j->cap ? j->cap * 2 : 16;
                j->v = embi_net_realloc(j->v, (size_t)ncap * sizeof *j->v);
                j->cap = ncap;
            }
            embi_cookie *c = &j->v[j->n++];
            *c = (embi_cookie){0};
            c->name = embi_net_strndup(name.ptr, name.len);
            c->value = embi_net_strndup(value.ptr, value.len);
            c->domain = embi_net_strndup(domain.data, domain.len);
            c->path = embi_net_strndup(path.data, path.len);
            c->host_only = host_only;
            c->secure = a.secure;
            c->http_only = a.http_only;
            c->samesite = a.samesite;
            c->expiry_ms = expiry;
            c->created_ms = created;
            c->seq = seq;
            changed = true;
        }
    }
    embi_sb_free(&domain);
    embi_sb_free(&path);
    embi_sb_free(&a.domain);
    embi_sb_free(&a.path);
    return changed;
}

/* ------------------------------------------------------------------ header */

static int cmp_cookie(const void *pa, const void *pb)
{
    const embi_cookie *x = *(const embi_cookie *const *)pa;
    const embi_cookie *y = *(const embi_cookie *const *)pb;
    size_t lx = strlen(x->path), ly = strlen(y->path);
    if (lx != ly) {
        return lx > ly ? -1 : 1;
    }
    if (x->created_ms != y->created_ms) {
        return x->created_ms < y->created_ms ? -1 : 1;
    }
    return x->seq < y->seq ? -1 : (x->seq > y->seq ? 1 : 0);
}

void embi_cookiejar_header(embi_cookiejar *j, const embi_url *url, bool include_http_only, int64_t now_ms, embi_sb *out)
{
    embi_str host = {url->href + url->host_off, url->host_len};
    embi_str path = url_path(url);
    bool https = url_https(url);
    for (uint32_t i = 0; i < j->n;) { /* expired cookies are dropped as they are met */
        if (j->v[i].expiry_ms <= now_ms) {
            remove_at(j, i);
        } else {
            i++;
        }
    }
    if (j->n == 0) {
        return;
    }
    const embi_cookie **m = embi_net_zalloc(sizeof(*m) * j->n);
    uint32_t nm = 0;
    for (uint32_t i = 0; i < j->n; i++) {
        const embi_cookie *c = &j->v[i];
        bool dom = c->host_only ? (strlen(c->domain) == host.len && memcmp(c->domain, host.ptr, host.len) == 0)
                                : domain_match(host.ptr, host.len, c->domain);
        if (!dom || !path_match(path.ptr, path.len, c->path) || (c->secure && !https) ||
            (c->http_only && !include_http_only)) {
            continue;
        }
        m[nm++] = c;
    }
    if (nm > 1) {
        qsort((void *)m, nm, sizeof *m, cmp_cookie);
    }
    for (uint32_t i = 0; i < nm; i++) {
        if (i > 0) {
            embi_sb_append(out, EMBI_STR("; "));
        }
        const embi_cookie *c = m[i];
        if (c->name[0] != '\0') {
            embi_sb_append(out, (embi_str){c->name, (uint32_t)strlen(c->name)});
            embi_sb_append_char(out, '=');
        }
        embi_sb_append(out, (embi_str){c->value, (uint32_t)strlen(c->value)});
    }
    embi_net_mfree((void *)m);
}
