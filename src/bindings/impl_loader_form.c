/*
 * FormData (fetch.webidl, W7c), its multipart/form-data serialization (a Request or Response body init) and the parsing of
 * urlencoded and multipart bodies (Body.formData()). The entries are a JS array in the wrapper's ref slot: name, value,
 * name, value ... where a value is a string or a File wrapper. The wrapper's block is empty (only the ref slot is used).
 *
 * new FormData(formElement) is in impl_forms.c (W5e).
 */
#include "bindings/impl_loader.h"

#include "bindings/gen/fetch.h"
#include "bindings/impl_forms.h"

static embi_js_value entries_of(embi_bind_obj *bo)
{
    return bo->ref;
}

bool ldr_is_formdata(embi_js_value v)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    return bo != nullptr && bo->iface == EMBI_IF_FormData && !embi_bind_stale(bo);
}

static embi_js_value fd_new(embi_bind_ctx *c)
{
    (void)ldr_state_of(c);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_FormData, c, nullptr, 0);
    embi_bind_set_ref(embi_bind_obj_of(v), embi_bind_array_new(c, 0));
    return v;
}

static void fd_push(embi_bind_ctx *c, embi_bind_obj *bo, embi_js_value name, embi_js_value value)
{
    embi_js_value arr = entries_of(bo);
    uint32_t n = embi_bind_array_len(c, arr);
    embi_bind_array_set(c, arr, n, name);
    embi_bind_array_set(c, arr, n + 1u, value);
}

embi_js_value embi_impl_FormData_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    if (!embi_js_is_undefined(a0)) {
        /* new FormData(form): the entries of a form element (W5e, impl_forms.c); anything else is a TypeError there. */
        return embi_bind_leave(sc, embi_forms_formdata_construct(c, new_target, a0));
    }
    embi_js_value v = fd_new(c);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

/* The entry value for append and set: a string, or a File (a Blob becomes a File named `filename` or "blob"). */
static embi_js_value entry_value(embi_bind_ctx *c, embi_js_value value, embi_js_value filename)
{
    const uint8_t *p;
    size_t n;
    embi_str type;
    if (ldr_blob_view(value, &p, &n, &type)) {
        embi_str name;
        double lm;
        if (ldr_file_info(value, &name, &lm) && embi_js_is_undefined(filename)) {
            return value;
        }
        embi_sb fn = {nullptr, 0, 0};
        if (!embi_js_is_undefined(filename)) {
            if (!ldr_to_utf8(c, filename, &fn)) {
                embi_sb_free(&fn);
                return EMBI_JS_EXCEPTION;
            }
        } else {
            embi_sb_append(&fn, EMBI_STR("blob"));
        }
        embi_js_value f = ldr_file_new(c, p, n, type, (embi_str){fn.data, fn.len}, 0);
        embi_sb_free(&fn);
        return f;
    }
    embi_sb s = {nullptr, 0, 0};
    if (!ldr_to_utf8(c, value, &s)) {
        embi_sb_free(&s);
        return EMBI_JS_EXCEPTION;
    }
    embi_js_value v = embi_bind_utf8_value(c, (embi_str){s.data, s.len});
    embi_sb_free(&s);
    return v;
}

embi_dom_err embi_impl_FormData_append(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value a1, embi_js_value a2)
{
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_js_value v = entry_value(c, a1, a2);
    if (v == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    fd_push(c, self, embi_bind_dstr_value(c, a0), v);
    return EMBI_DOM_OK;
}

/* Removes the entries named name (UTF-8) from the array; when keep_first is set the first match stays and its value is
 * replaced by `replace` (FormData.set). Returns true when something matched. */
static bool fd_filter(embi_bind_ctx *c, embi_bind_obj *self, embi_str name, bool set, embi_js_value replace)
{
    embi_js_value arr = entries_of(self);
    uint32_t n = embi_bind_array_len(c, arr);
    embi_js_value out = embi_bind_array_new(c, 0);
    uint32_t w = 0;
    bool matched = false;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        embi_js_value k = embi_bind_array_get(c, arr, i);
        embi_js_value v = embi_bind_array_get(c, arr, i + 1);
        embi_sb ks = {nullptr, 0, 0};
        bool eq = ldr_to_utf8(c, k, &ks) && embi_str_eq((embi_str){ks.data, ks.len}, name);
        embi_sb_free(&ks);
        if (eq) {
            if (set && !matched) {
                v = replace;
            } else {
                matched = true;
                continue;
            }
            matched = true;
        }
        embi_bind_array_set(c, out, w++, k);
        embi_bind_array_set(c, out, w++, v);
    }
    embi_bind_set_ref(self, out);
    return matched;
}

void embi_impl_FormData_delete(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    if (!embi_bind_stale(self)) {
        (void)fd_filter(c, self, embi_bind_utf8(c, a0), false, EMBI_JS_UNDEFINED);
    }
}

embi_dom_err embi_impl_FormData_set(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, embi_js_value a1, embi_js_value a2)
{
    if (embi_bind_stale(self)) {
        return EMBI_DOM_OK;
    }
    embi_js_value v = entry_value(c, a1, a2);
    if (v == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    if (!fd_filter(c, self, embi_bind_utf8(c, a0), true, v)) {
        fd_push(c, self, embi_bind_dstr_value(c, a0), v);
    }
    return EMBI_DOM_OK;
}

/* The index of the first entry named name, or UINT32_MAX. count_all collects the values into out instead. */
static uint32_t fd_find(embi_bind_ctx *c, embi_bind_obj *self, embi_str name)
{
    embi_js_value arr = entries_of(self);
    uint32_t n = embi_bind_array_len(c, arr);
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        embi_sb ks = {nullptr, 0, 0};
        bool eq = ldr_to_utf8(c, embi_bind_array_get(c, arr, i), &ks) && embi_str_eq((embi_str){ks.data, ks.len}, name);
        embi_sb_free(&ks);
        if (eq) {
            return i;
        }
    }
    return UINT32_MAX;
}

embi_js_value embi_impl_FormData_get(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    if (embi_bind_stale(self)) {
        return EMBI_JS_NULL;
    }
    uint32_t i = fd_find(c, self, embi_bind_utf8(c, a0));
    return i == UINT32_MAX ? EMBI_JS_NULL : embi_bind_array_get(c, entries_of(self), i + 1u);
}

bool embi_impl_FormData_has(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    return !embi_bind_stale(self) && fd_find(c, self, embi_bind_utf8(c, a0)) != UINT32_MAX;
}

embi_bind_seq embi_impl_FormData_getAll(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_bind_seq out = {nullptr, 0};
    if (embi_bind_stale(self)) {
        return out;
    }
    embi_str name = embi_bind_utf8(c, a0);
    embi_js_value arr = entries_of(self);
    uint32_t n = embi_bind_array_len(c, arr);
    embi_js_value *vals = (embi_js_value *)embi_bind_alloc(c, (size_t)(n / 2u + 1u) * sizeof *vals);
    uint32_t k = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        embi_sb ks = {nullptr, 0, 0};
        bool eq = ldr_to_utf8(c, embi_bind_array_get(c, arr, i), &ks) && embi_str_eq((embi_str){ks.data, ks.len}, name);
        embi_sb_free(&ks);
        if (eq) {
            vals[k++] = embi_bind_array_get(c, arr, i + 1);
        }
    }
    out.data = k > 0 ? vals : nullptr;
    out.len = k;
    return out;
}

uint32_t embi_impl_FormData_iter_size(embi_bind_ctx *c, embi_bind_obj *self)
{
    return embi_bind_stale(self) ? 0 : embi_bind_array_len(c, entries_of(self)) / 2u;
}

embi_js_value embi_impl_FormData_iter_key(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    return embi_bind_array_get(c, entries_of(self), i * 2u);
}

embi_js_value embi_impl_FormData_iter_value(embi_bind_ctx *c, embi_bind_obj *self, uint32_t i)
{
    return embi_bind_array_get(c, entries_of(self), i * 2u + 1u);
}

/* ------------------------------------------------------------------ multipart serialization */

/* Appends a name or filename escaped for a Content-Disposition parameter (newlines normalized, quote and CR LF escaped). */
static void append_escaped(embi_sb *out, embi_str s)
{
    for (uint32_t i = 0; i < s.len; i++) {
        char ch = s.ptr[i];
        if (ch == '"') {
            embi_sb_append(out, EMBI_STR("%22"));
        } else if (ch == '\r' && i + 1 < s.len && s.ptr[i + 1] == '\n') {
            embi_sb_append(out, EMBI_STR("%0D%0A"));
            i++;
        } else if (ch == '\r') {
            embi_sb_append(out, EMBI_STR("%0D%0A"));
        } else if (ch == '\n') {
            embi_sb_append(out, EMBI_STR("%0D%0A"));
        } else {
            embi_sb_append_char(out, ch);
        }
    }
}

/* Appends s with every newline normalized to CRLF. */
static void append_crlf(embi_sb *out, embi_str s)
{
    for (uint32_t i = 0; i < s.len; i++) {
        char ch = s.ptr[i];
        if (ch == '\r' && i + 1 < s.len && s.ptr[i + 1] == '\n') {
            embi_sb_append(out, EMBI_STR("\r\n"));
            i++;
        } else if (ch == '\r' || ch == '\n') {
            embi_sb_append(out, EMBI_STR("\r\n"));
        } else {
            embi_sb_append_char(out, ch);
        }
    }
}

bool ldr_formdata_multipart(embi_bind_ctx *c, embi_js_value v, embi_sb *out, embi_sb *ctype)
{
    embi_bind_obj *bo = embi_bind_obj_of(v);
    if (bo == nullptr || bo->iface != EMBI_IF_FormData || embi_bind_stale(bo)) {
        return false;
    }
    uint8_t rnd[8];
    embi_rng_bytes(ldr_rng(), rnd, sizeof rnd);
    char boundary[48];
    int bl = SDL_snprintf(boundary, sizeof boundary, "----EmberFormBoundary%02x%02x%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1],
                          rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
    embi_js_value arr = entries_of(bo);
    uint32_t n = embi_bind_array_len(c, arr);
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        embi_sb name = {nullptr, 0, 0};
        (void)ldr_to_utf8(c, embi_bind_array_get(c, arr, i), &name);
        embi_js_value val = embi_bind_array_get(c, arr, i + 1);
        embi_sb_append(out, EMBI_STR("--"));
        embi_sb_append(out, (embi_str){boundary, (uint32_t)bl});
        embi_sb_append(out, EMBI_STR("\r\nContent-Disposition: form-data; name=\""));
        append_escaped(out, (embi_str){name.data, name.len});
        embi_sb_append(out, EMBI_STR("\""));
        const uint8_t *p;
        size_t len;
        embi_str type, fname;
        double lm;
        if (ldr_blob_view(val, &p, &len, &type) && ldr_file_info(val, &fname, &lm)) {
            embi_sb_append(out, EMBI_STR("; filename=\""));
            append_escaped(out, fname);
            embi_sb_append(out, EMBI_STR("\"\r\nContent-Type: "));
            if (type.len > 0) {
                embi_sb_append(out, type);
            } else {
                embi_sb_append(out, EMBI_STR("application/octet-stream"));
            }
            embi_sb_append(out, EMBI_STR("\r\n\r\n"));
            if (len > 0) {
                embi_sb_append(out, (embi_str){(const char *)p, (uint32_t)len});
            }
        } else {
            embi_sb s = {nullptr, 0, 0};
            (void)ldr_to_utf8(c, val, &s);
            embi_sb_append(out, EMBI_STR("\r\n\r\n"));
            append_crlf(out, (embi_str){s.data, s.len});
            embi_sb_free(&s);
        }
        embi_sb_append(out, EMBI_STR("\r\n"));
        embi_sb_free(&name);
    }
    /* The closing delimiter is written for an empty FormData too (the multipart/form-data encoding algorithm; WPT
     * response-form-data "Empty form data" needs it). request-consume-empty and response-consume-empty expect "" for the
     * same input: the two expectations contradict each other; the delimiter wins and the consume-empty FormData subsets are
     * labelled not-applicable (W7f). */
    embi_sb_append(out, EMBI_STR("--"));
    embi_sb_append(out, (embi_str){boundary, (uint32_t)bl});
    embi_sb_append(out, EMBI_STR("--\r\n"));
    embi_sb_append(ctype, EMBI_STR("multipart/form-data; boundary="));
    embi_sb_append(ctype, (embi_str){boundary, (uint32_t)bl});
    return true;
}

/* ------------------------------------------------------------------ parsing */

static const char *find_bytes(const char *h, size_t hn, const char *needle, size_t nn)
{
    if (nn == 0 || hn < nn) {
        return nullptr;
    }
    for (size_t i = 0; i + nn <= hn; i++) {
        if (h[i] == needle[0] && memcmp(h + i, needle, nn) == 0) {
            return h + i;
        }
    }
    return nullptr;
}

/* The value of a quoted parameter (name="value") in a header line, or {nullptr, 0}. */
static embi_str disp_param(embi_str line, const char *name)
{
    char key[32];
    int kl = SDL_snprintf(key, sizeof key, "%s=\"", name);
    const char *at = find_bytes(line.ptr, line.len, key, (size_t)kl);
    while (at != nullptr && at != line.ptr && at[-1] != ' ' && at[-1] != ';') {
        size_t off = (size_t)(at - line.ptr) + 1;
        at = find_bytes(line.ptr + off, line.len - off, key, (size_t)kl);
    }
    if (at == nullptr) {
        return (embi_str){nullptr, 0};
    }
    const char *start = at + kl;
    const char *end = line.ptr + line.len;
    const char *q = start;
    while (q < end && *q != '"') {
        q++;
    }
    return (embi_str){start, (uint32_t)(q - start)};
}

/* The start of the header line whose name is name (lower case, with the colon), matched without case, or nullptr. */
static const char *find_header(embi_str headers, const char *name, size_t nl)
{
    const char *p = headers.ptr;
    const char *end = headers.ptr + headers.len;
    while (p < end) {
        if ((size_t)(end - p) >= nl && SDL_strncasecmp(p, name, nl) == 0) {
            return p;
        }
        const char *nlp = find_bytes(p, (size_t)(end - p), "\r\n", 2);
        if (nlp == nullptr) {
            break;
        }
        p = nlp + 2;
    }
    return nullptr;
}

static embi_js_value pct_unescape(embi_bind_ctx *c, embi_str s)
{
    embi_sb out = {nullptr, 0, 0};
    for (uint32_t i = 0; i < s.len; i++) {
        if (s.ptr[i] == '%' && i + 2 < s.len + 0 && s.ptr[i + 1] == '2' && s.ptr[i + 2] == '2') {
            embi_sb_append_char(&out, '"');
            i += 2;
        } else {
            embi_sb_append_char(&out, s.ptr[i]);
        }
    }
    embi_js_value v = embi_bind_utf8_value(c, (embi_str){out.data, out.len});
    embi_sb_free(&out);
    return v;
}

static embi_js_value parse_multipart(embi_bind_ctx *c, const char *body, size_t n, embi_str boundary)
{
    embi_sb delim = {nullptr, 0, 0};
    embi_sb_append(&delim, EMBI_STR("--"));
    embi_sb_append(&delim, boundary);
    embi_js_value fd = fd_new(c);
    embi_bind_obj *fb = embi_bind_obj_of(fd);
    const char *end = body != nullptr ? body + n : body; /* an empty body can be a null pointer */
    const char *p = find_bytes(body, n, delim.data, delim.len);
    bool ok = p != nullptr;
    while (ok) {
        p += delim.len;
        if (end - p >= 2 && p[0] == '-' && p[1] == '-') {
            /* The closing delimiter: only transport padding and a line break (then an epilogue) may follow it. */
            p += 2;
            while (p < end && (*p == ' ' || *p == '\t')) {
                p++;
            }
            if (p < end && !(end - p >= 2 && p[0] == '\r' && p[1] == '\n')) {
                ok = false;
            }
            break;
        }
        /* Transport padding (spaces and tabs) may follow the delimiter before its line break. */
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (end - p < 2 || p[0] != '\r' || p[1] != '\n') {
            ok = false;
            break;
        }
        p += 2;
        const char *hend = find_bytes(p, (size_t)(end - p), "\r\n\r\n", 4);
        if (hend == nullptr) {
            ok = false;
            break;
        }
        embi_str headers = {p, (uint32_t)(hend - p)};
        const char *data = hend + 4;
        /* The part ends at CRLF + delimiter. */
        embi_sb next = {nullptr, 0, 0};
        embi_sb_append(&next, EMBI_STR("\r\n"));
        embi_sb_append(&next, (embi_str){delim.data, delim.len});
        const char *dend = find_bytes(data, (size_t)(end - data), next.data, next.len);
        embi_sb_free(&next);
        if (dend == nullptr) {
            ok = false;
            break;
        }
        embi_str name = disp_param(headers, "name");
        embi_str fname = disp_param(headers, "filename");
        bool is_file = fname.ptr != nullptr; /* a filename parameter of its own, not a name that contains the text */
        embi_js_value nv = pct_unescape(c, name);
        embi_js_value val;
        if (is_file) {
            embi_str ctype = {nullptr, 0};
            const char *ct = find_header(headers, "content-type:", 13);
            if (ct != nullptr) {
                const char *s = ct + 13;
                while (s < headers.ptr + headers.len && *s == ' ') {
                    s++;
                }
                const char *e = s;
                while (e < headers.ptr + headers.len && *e != '\r' && *e != '\n') {
                    e++;
                }
                ctype = (embi_str){s, (uint32_t)(e - s)};
            }
            embi_sb fnn = {nullptr, 0, 0};
            for (uint32_t i = 0; i < fname.len; i++) {
                if (fname.ptr[i] == '%' && i + 2 < fname.len + 0 && fname.ptr[i + 1] == '2' && fname.ptr[i + 2] == '2') {
                    embi_sb_append_char(&fnn, '"');
                    i += 2;
                } else {
                    embi_sb_append_char(&fnn, fname.ptr[i]);
                }
            }
            val = ldr_file_new(c, (const uint8_t *)data, (size_t)(dend - data), ctype, (embi_str){fnn.data, fnn.len}, 0);
            embi_sb_free(&fnn);
        } else {
            val = embi_bind_utf8_value(c, (embi_str){data, (uint32_t)(dend - data)});
        }
        if (val == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
            ok = false;
            break;
        }
        fd_push(c, fb, nv, val);
        p = dend + 2; /* at the delimiter */
    }
    embi_sb_free(&delim);
    return ok ? fd : EMBI_JS_NULL;
}

embi_js_value ldr_formdata_parse(embi_bind_ctx *c, const uint8_t *p, size_t n, embi_str content_type)
{
    embi_mime m = {0};
    if (!embi_mime_parse(content_type, &m)) {
        return EMBI_JS_NULL;
    }
    embi_sb essence = {nullptr, 0, 0};
    embi_mime_essence(&m, &essence);
    embi_js_value result = EMBI_JS_NULL;
    if (embi_str_eq((embi_str){essence.data, essence.len}, EMBI_STR("application/x-www-form-urlencoded"))) {
        embi_form f = {0};
        embi_form_parse((embi_str){(const char *)p, (uint32_t)n}, &f);
        result = fd_new(c);
        embi_bind_obj *fb = embi_bind_obj_of(result);
        for (uint32_t i = 0; i < f.len; i++) {
            fd_push(c, fb, embi_bind_utf8_value(c, (embi_str){f.data[i].name.data, f.data[i].name.len}),
                    embi_bind_utf8_value(c, (embi_str){f.data[i].value.data, f.data[i].value.len}));
        }
        embi_form_free(&f);
    } else if (embi_str_eq((embi_str){essence.data, essence.len}, EMBI_STR("multipart/form-data"))) {
        embi_str boundary = embi_mime_param_value(&m, EMBI_STR("boundary"));
        if (boundary.len > 0) {
            result = parse_multipart(c, (const char *)p, n, boundary);
        }
    }
    embi_sb_free(&essence);
    embi_mime_free(&m);
    return result;
}
