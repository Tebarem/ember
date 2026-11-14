/*
 * MIME types (MIME Sniffing 4.2 parsing and serialization), the JavaScript and JSON essence
 * checks, and the extension table used for handlers that return no type.
 */
#include "loader/loader.h"

#include <string.h>

static bool http_ws(uint32_t c)
{
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

/* HTTP token code points (RFC 7230): token chars are ASCII only. */
static bool token_cp(uint32_t c)
{
    if (c >= '0' && c <= '9') {
        return true;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        return true;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-':
    case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

/* HTTP quoted-string token code points: TAB, printable ASCII and Latin-1. */
static bool quoted_token_cp(uint32_t c)
{
    return c == 0x09 || (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF);
}

static uint32_t lower_ascii(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* A byte cursor over the input. Code points are decoded for the checks, and the cursor moves in
 * bytes. Every cut point is at an ASCII byte, so slices stay valid UTF-8. */
typedef struct cursor {
    const char *p;
    size_t n;
    size_t pos;
} cursor;

static bool at_end(const cursor *c)
{
    return c->pos >= c->n;
}

static unsigned char peek(const cursor *c)
{
    return (unsigned char)c->p[c->pos];
}

/* Collects bytes until a stop byte (or the end) and returns the slice. */
static embi_str collect_until(cursor *c, unsigned char stop1, unsigned char stop2)
{
    size_t s = c->pos;
    while (c->pos < c->n && (unsigned char)c->p[c->pos] != stop1 &&
           (unsigned char)c->p[c->pos] != stop2) {
        c->pos++;
    }
    embi_str r = {c->p, (uint32_t)(c->pos - s)};
    if (r.len > 0) {
        r.ptr = c->p + s;
    }
    return r;
}

static bool all_tokens(embi_str s)
{
    const char *p = s.ptr;
    const char *e = embi_str_end(s);
    if (s.len == 0) {
        return false;
    }
    while (p < e) {
        if (!token_cp(embi_utf8_decode(&p, e))) {
            return false;
        }
    }
    return true;
}

static bool all_quoted_tokens(embi_str s)
{
    const char *p = s.ptr;
    const char *e = embi_str_end(s);
    while (p < e) {
        if (!quoted_token_cp(embi_utf8_decode(&p, e))) {
            return false;
        }
    }
    return true;
}

static void strip_http_ws(embi_str *s)
{
    while (s->len > 0 && http_ws((unsigned char)s->ptr[0])) {
        s->ptr++;
        s->len--;
    }
    while (s->len > 0 && http_ws((unsigned char)s->ptr[s->len - 1])) {
        s->len--;
    }
}

static void append_lower(embi_sb *sb, embi_str s)
{
    for (uint32_t i = 0; i < s.len; i++) {
        embi_sb_append_char(sb, (char)lower_ascii((unsigned char)s.ptr[i]));
    }
}

/* HTTP quoted-string with extract-value: the cursor is at the opening quote. */
static void collect_quoted(cursor *c, embi_sb *value)
{
    c->pos++; /* opening quote */
    while (!at_end(c)) {
        size_t s = c->pos;
        while (c->pos < c->n && c->p[c->pos] != '"' && c->p[c->pos] != '\\') {
            c->pos++;
        }
        if (c->pos > s) {
            embi_sb_append(value, (embi_str){c->p + s, (uint32_t)(c->pos - s)});
        }
        if (at_end(c)) {
            break;
        }
        char q = c->p[c->pos];
        c->pos++;
        if (q == '\\') {
            if (at_end(c)) {
                embi_sb_append_char(value, '\\');
                break;
            }
            /* The escaped byte; a multi-byte code point is copied whole below. */
            size_t cs = c->pos;
            const char *p = c->p + cs;
            const char *e = c->p + c->n;
            embi_utf8_decode(&p, e);
            embi_sb_append(value, (embi_str){c->p + cs, (uint32_t)(p - (c->p + cs))});
            c->pos = (size_t)(p - c->p);
        } else {
            /* A closing quote. */
            break;
        }
    }
}

void embi_mime_free(embi_mime *m)
{
    embi_sb_free(&m->type);
    embi_sb_free(&m->subtype);
    for (uint32_t i = 0; i < m->params.len; i++) {
        embi_sb_free(&m->params.data[i].name);
        embi_sb_free(&m->params.data[i].value);
    }
    embi_vec_free(&m->params);
    *m = (embi_mime){0};
}

bool embi_mime_parse(embi_str input, embi_mime *out)
{
    *out = (embi_mime){0};
    embi_str s = input;
    strip_http_ws(&s);
    cursor c = {s.ptr, s.len, 0};

    embi_str type = collect_until(&c, '/', '/');
    if (type.len == 0 || !all_tokens(type) || at_end(&c)) {
        return false;
    }
    c.pos++; /* '/' */
    embi_str subtype = collect_until(&c, ';', ';');
    /* Only trailing HTTP whitespace is removed from the subtype (MIME Sniffing 4.2 step 9). */
    while (subtype.len > 0 && http_ws((unsigned char)subtype.ptr[subtype.len - 1])) {
        subtype.len--;
    }
    if (subtype.len == 0 || !all_tokens(subtype)) {
        return false;
    }
    append_lower(&out->type, type);
    append_lower(&out->subtype, subtype);

    while (!at_end(&c)) {
        c.pos++; /* ';' */
        while (!at_end(&c) && http_ws(peek(&c))) {
            c.pos++;
        }
        embi_str name = collect_until(&c, ';', '=');
        embi_sb lname = {nullptr, 0, 0};
        append_lower(&lname, name);
        if (!at_end(&c)) {
            if (peek(&c) == ';') {
                embi_sb_free(&lname);
                continue;
            }
            c.pos++; /* '=' */
        }
        if (at_end(&c)) {
            embi_sb_free(&lname);
            break;
        }
        embi_sb value = {nullptr, 0, 0};
        if (peek(&c) == '"') {
            collect_quoted(&c, &value);
            /* Discard anything after the closing quote, up to the next ';'. */
            collect_until(&c, ';', ';');
        } else {
            embi_str raw = collect_until(&c, ';', ';');
            /* Only trailing HTTP whitespace goes from an unquoted value; leading whitespace stays. */
            while (raw.len > 0 && http_ws((unsigned char)raw.ptr[raw.len - 1])) {
                raw.len--;
            }
            if (raw.len == 0) {
                embi_sb_free(&lname);
                embi_sb_free(&value);
                continue;
            }
            embi_sb_append(&value, raw);
        }

        bool valid_name = lname.len > 0 && all_tokens((embi_str){lname.data, lname.len});
        bool valid_value = all_quoted_tokens((embi_str){value.data, value.len});
        bool dup = false;
        for (uint32_t i = 0; i < out->params.len; i++) {
            if (out->params.data[i].name.len == lname.len &&
                memcmp(out->params.data[i].name.data, lname.data, lname.len) == 0) {
                dup = true;
                break;
            }
        }
        if (valid_name && valid_value && !dup) {
            embi_mime_param p = {{nullptr, 0, 0}, {nullptr, 0, 0}};
            p.name = lname;
            p.value = value;
            embi_vec_push(&out->params, p);
        } else {
            embi_sb_free(&lname);
            embi_sb_free(&value);
        }
    }
    return true;
}

/* Writes a parameter value, quoted when it is not a plain token. */
static void serialize_value(embi_sb *out, const embi_sb *v)
{
    bool plain = v->len > 0 && all_tokens((embi_str){v->data, v->len});
    if (plain) {
        embi_sb_append(out, (embi_str){v->data, v->len});
        return;
    }
    embi_sb_append_char(out, '"');
    for (uint32_t i = 0; i < v->len; i++) {
        char c = v->data[i];
        if (c == '"' || c == '\\') {
            embi_sb_append_char(out, '\\');
        }
        embi_sb_append_char(out, c);
    }
    embi_sb_append_char(out, '"');
}

void embi_mime_serialize(const embi_mime *m, embi_sb *out)
{
    embi_sb_append(out, (embi_str){m->type.data, m->type.len});
    embi_sb_append_char(out, '/');
    embi_sb_append(out, (embi_str){m->subtype.data, m->subtype.len});
    for (uint32_t i = 0; i < m->params.len; i++) {
        const embi_mime_param *p = &m->params.data[i];
        embi_sb_append_char(out, ';');
        embi_sb_append(out, (embi_str){p->name.data, p->name.len});
        embi_sb_append_char(out, '=');
        serialize_value(out, &p->value);
    }
}

void embi_mime_essence(const embi_mime *m, embi_sb *out)
{
    embi_sb_append(out, (embi_str){m->type.data, m->type.len});
    embi_sb_append_char(out, '/');
    embi_sb_append(out, (embi_str){m->subtype.data, m->subtype.len});
}

embi_str embi_mime_param_value(const embi_mime *m, embi_str name)
{
    for (uint32_t i = 0; i < m->params.len; i++) {
        const embi_mime_param *p = &m->params.data[i];
        if (p->name.len == name.len && (name.len == 0 || memcmp(p->name.data, name.ptr, name.len) == 0)) {
            return (embi_str){p->value.data, p->value.len};
        }
    }
    return (embi_str){nullptr, 0};
}

/* The essences of the HTML script types that the module and classic loaders accept. */
static const char *const js_essences[] = {
    "application/ecmascript", "application/javascript", "application/x-ecmascript",
    "application/x-javascript", "text/ecmascript", "text/javascript", "text/javascript1.0",
    "text/javascript1.1", "text/javascript1.2", "text/javascript1.3", "text/javascript1.4",
    "text/javascript1.5", "text/jscript", "text/livescript", "text/x-ecmascript",
    "text/x-javascript",
};

bool embi_mime_essence_is_javascript(embi_str essence)
{
    for (size_t i = 0; i < sizeof js_essences / sizeof js_essences[0]; i++) {
        if (embi_str_ieq(essence, (embi_str){js_essences[i], (uint32_t)strlen(js_essences[i])})) {
            return true;
        }
    }
    return false;
}

bool embi_mime_essence_is_json(embi_str essence)
{
    if (embi_str_ieq(essence, EMBI_STR("application/json")) || embi_str_ieq(essence, EMBI_STR("text/json"))) {
        return true;
    }
    static const char suffix[] = "+json";
    uint32_t sl = (uint32_t)(sizeof suffix - 1);
    const char *slash = essence.ptr != nullptr ? memchr(essence.ptr, '/', essence.len) : nullptr;
    if (slash == nullptr || slash == essence.ptr) {
        return false;
    }
    /* Any subtype that ends in "+json", including "+json" itself (MIME Sniffing). */
    uint32_t sub_off = (uint32_t)(slash - essence.ptr) + 1;
    if (essence.len < sub_off + sl) {
        return false;
    }
    return embi_str_ieq((embi_str){essence.ptr + essence.len - sl, sl}, EMBI_STR(suffix));
}

/* Extension to MIME essence. Extensions are lowercase here. */
static const struct {
    const char *ext;
    const char *mime;
} ext_table[] = {
    {"js", "text/javascript"},     {"mjs", "text/javascript"},     {"css", "text/css"},
    {"html", "text/html"},         {"json", "application/json"},   {"wasm", "application/wasm"},
    {"svg", "image/svg+xml"},      {"png", "image/png"},           {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},        {"gif", "image/gif"},           {"webp", "image/webp"},
    {"woff", "font/woff"},         {"woff2", "font/woff2"},        {"ttf", "font/ttf"},
    {"otf", "font/otf"},           {"txt", "text/plain"},          {"map", "application/json"},
};

embi_str embi_mime_from_path(embi_str path)
{
    uint32_t end = path.len;
    for (uint32_t i = 0; i < path.len; i++) {
        if (path.ptr[i] == '?' || path.ptr[i] == '#') {
            end = i;
            break;
        }
    }
    uint32_t dot = 0;
    uint32_t slash = 0;
    bool has_dot = false;
    for (uint32_t i = 0; i < end; i++) {
        if (path.ptr[i] == '/' || path.ptr[i] == '\\') {
            slash = i + 1;
            has_dot = false;
        } else if (path.ptr[i] == '.') {
            dot = i;
            has_dot = true;
        }
    }
    if (!has_dot || dot < slash || dot + 1 >= end) {
        return (embi_str){nullptr, 0};
    }
    embi_str ext = {path.ptr + dot + 1, end - dot - 1};
    for (size_t i = 0; i < sizeof ext_table / sizeof ext_table[0]; i++) {
        if (embi_str_ieq(ext, (embi_str){ext_table[i].ext, (uint32_t)strlen(ext_table[i].ext)})) {
            return (embi_str){ext_table[i].mime, (uint32_t)strlen(ext_table[i].mime)};
        }
    }
    return (embi_str){nullptr, 0};
}
