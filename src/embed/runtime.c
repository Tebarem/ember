/*
 * The runtime (W4e, ember.h section "runtime"): creation, the font registry entry point, resource handlers, the allowed
 * file directories, and the fetch that every load goes through. No network: only the built-in file: and data: loaders
 * and the handlers the embedder registered.
 */
#include "embed/url.h"
#include "embed_int.h"
#include "embed/resource_int.h"

#include "layout/text_shim.h"
#include "style/style.h"

#ifdef EMBER_NET
#include "net/net.h"
#endif

#include <stdarg.h>
#include <string.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define EMBI_RT_PATH_MAX 4096u

static double ms_since(uint64_t t0)
{
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

void embi_rt_log(emb_runtime *rt, emb_log_level level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (rt != nullptr && rt->log != nullptr) {
        rt->log(rt->log_user, level, buf);
    } else {
        SDL_Log("%s", buf);
    }
}

/* SZ3b: the registry says through the runtime's log that no font can draw some text (once). */
static void font_warn(void *user, const char *msg)
{
    embi_rt_log(user, EMB_LOG_WARN, "%s", msg);
}

#ifdef EMBER_NET
static void net_log(void *user, emb_log_level level, const char *message)
{
    embi_rt_log(user, level, "%s", message);
}
#endif

/* Parses the shared UA sheet off the caller's thread: the first view needs it, and parsing it is 1.3 ms. The thread
 * keeps one reference for the runtime's life so that the sheet survives a view-less moment. */
static int SDLCALL ua_prefetch_main(void *arg)
{
    emb_runtime *rt = arg;
    rt->ua_sheet = embi_style_ua_sheet();
    return 0;
}

emb_runtime *emb_runtime_create(const emb_runtime_desc *desc)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    emb_runtime *rt = embi_alloc(sizeof *rt);
    memset(rt, 0, sizeof *rt);
    if (desc != nullptr) {
        rt->log_user = desc->log_user;
        rt->log = desc->log;
    }
    if (desc != nullptr && (desc->flags & EMB_RUNTIME_NO_DEFAULT_FONT) != 0) {
        embi_rt_log(rt, EMB_LOG_ERROR, "emb_runtime_create: EMB_RUNTIME_NO_DEFAULT_FONT is not supported");
        embi_free(rt, sizeof *rt);
        return nullptr;
    }
    if (desc != nullptr) {
        rt->flags = desc->flags;
        rt->image_bytes = desc->image_cache_bytes;
        rt->log_user = desc->log_user;
        rt->log = desc->log;
    }
    /* The first shaping of this runtime is timed for the stats (the process-wide counter restarts here). */
    embi_tshim_reset_first();
    rt->text = embi_tshim_create();
    if (rt->text == nullptr) {
        embi_free(rt, sizeof *rt);
        return nullptr;
    }
    rt->fonts = embi_tshim_fonts(rt->text);
    embi_fonts_set_warn(rt->fonts, font_warn, rt);
    /* The environment can turn system fonts off for a whole process (the test harness does, so results stay machine
     * independent): EMBER_NO_SYSTEM_FONTS set to anything but empty or 0. */
    const char *nosys = SDL_getenv("EMBER_NO_SYSTEM_FONTS");
    if (nosys != nullptr && nosys[0] != '\0' && !(nosys[0] == '0' && nosys[1] == '\0')) {
        rt->flags |= EMB_RUNTIME_NO_SYSTEM_FONTS;
    }
    /* The system font index starts now and is built on a job thread: nothing waits for it (W4i). */
    embi_fonts_system_start(rt->fonts, (rt->flags & EMB_RUNTIME_NO_SYSTEM_FONTS) == 0);
#ifdef EMBER_NET
    rt->net = embi_net_create(net_log, rt);
#endif
    rt->ua_thread = SDL_CreateThread(ua_prefetch_main, "ember-ua", rt);
    rt->runtime_init_ms = ms_since(t0);
    return rt;
}

bool emb_runtime_set_generic_family(emb_runtime *rt, const char *generic, const char *const *families, size_t count)
{
    if (rt == nullptr) {
        return false;
    }
    return embi_fonts_generic(rt->fonts, generic, families, count);
}

void emb_runtime_trim_fonts(emb_runtime *rt)
{
    if (rt != nullptr) {
        embi_fonts_trim(rt->fonts, rt->text);
    }
}

void emb_runtime_stats_get(const emb_runtime *rt, emb_runtime_stats *out)
{
    if (out == nullptr || out->size < sizeof(emb_runtime_stats) || rt == nullptr) {
        return;
    }
    embi_fonts_stats(rt->fonts, (rt->flags & EMB_RUNTIME_NO_SYSTEM_FONTS) == 0, out);
}

void emb_runtime_destroy(emb_runtime *rt)
{
    if (rt == nullptr) {
        return;
    }
#ifdef EMBER_NET
    /* The network thread stops first: requests in flight end (their release callbacks run) before anything they might
     * touch goes away. */
    embi_net_destroy(rt->net);
    rt->net = nullptr;
#endif
    /* Each registration's live requests are detached (v2_free): aborted, and their handlers are asked to complete them
     * (the cancel function). A completion that comes later, or the one the cancel makes, frees the request and does not
     * touch the runtime. */
    for (uint32_t i = 0; i < EMBI_RT_MAX_HANDLERS; i++) {
        SDL_free(rt->handlers[i].scheme);
        if (rt->handlers[i].fn == embi_v2_tramp) {
            embi_v2_free(rt->handlers[i].user);
        }
    }
    for (uint32_t i = 0; i < rt->ndirs; i++) {
        SDL_free(rt->dirs[i]);
    }
    embi_rt_extras_free(rt);
    if (rt->ua_thread != nullptr) {
        SDL_WaitThread(rt->ua_thread, nullptr);
        rt->ua_thread = nullptr;
        embi_style_ua_release(rt->ua_sheet);
        rt->ua_sheet = nullptr;
    }
    embi_tshim_destroy(rt->text);
    embi_free(rt, sizeof *rt);
}

bool emb_runtime_add_font(emb_runtime *rt, const void *data, size_t len)
{
    if (rt == nullptr || data == nullptr || len == 0) {
        return false;
    }
    uint8_t *copy = SDL_malloc(len);
    if (copy == nullptr) {
        return false;
    }
    SDL_memcpy(copy, data, len);
    /* The registry takes the bytes (owned) and frees them with SDL_free. */
    uint32_t first_new = embi_fonts_face_count(rt->fonts);
    if (embi_fonts_add(rt->fonts, copy, len, true) == 0) {
        embi_rt_log(rt, EMB_LOG_WARN, "emb_runtime_add_font: no usable face in %u bytes", (unsigned)len);
        return false;
    }
    embi_fonts_default_if_none(rt->fonts, first_new); /* no bundled default (EMBER_DEFAULT_FONT=OFF): this one is it */
    return true;
}

static bool scheme_valid(const char *s, size_t n)
{
    if (n == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' ||
                  c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Sets the handler of scheme (the v1 function and its user; a v2 slot passes v2_tramp and its record). A replaced or
 * removed v2 record is freed. Returns false for a bad scheme, and then nothing changes. */
bool embi_rt_handler_set(emb_runtime *rt, const char *scheme, emb_resource_fn fn, void *user)
{
    if (rt == nullptr || scheme == nullptr) {
        return false;
    }
    size_t n = strlen(scheme);
    if (!scheme_valid(scheme, n)) {
        return false;
    }
    embi_str sv = {scheme, (uint32_t)n};
    /* blob: is always served from the runtime's registry (the same for a view load and for fetch). */
    if (embi_url_scheme_is(sv, "file") || embi_url_scheme_is(sv, "data") || embi_url_scheme_is(sv, "blob")) {
        return false;
    }
    /* An existing slot of the scheme is replaced (or removed when fn is null). */
    int slot = -1;
    for (uint32_t i = 0; i < EMBI_RT_MAX_HANDLERS; i++) {
        embi_rt_handler *h = &rt->handlers[i];
        if (h->scheme != nullptr && embi_url_scheme_is(sv, h->scheme)) {
            slot = (int)i;
            break;
        }
        if (h->scheme == nullptr && slot < 0 && fn != nullptr) {
            slot = (int)i;
        }
    }
    if (slot < 0) {
        return false;
    }
    embi_rt_handler *h = &rt->handlers[slot];
    if (fn == nullptr) {
        SDL_free(h->scheme);
        if (h->fn == embi_v2_tramp) {
            embi_v2_free(h->user);
        }
        *h = (embi_rt_handler){nullptr, nullptr, nullptr};
        return true;
    }
    if (h->fn == embi_v2_tramp && h->user != user) {
        embi_v2_free(h->user);
    }
    if (h->scheme == nullptr) {
        h->scheme = SDL_malloc(n + 1);
        for (size_t i = 0; i < n; i++) {
            char c = scheme[i];
            h->scheme[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        h->scheme[n] = '\0';
    }
    h->fn = fn;
    h->user = user;
    return true;
}

bool emb_runtime_set_resource_handler(emb_runtime *rt, const char *scheme, emb_resource_fn fn, void *user)
{
    return embi_rt_handler_set(rt, scheme, fn, user);
}

bool emb_runtime_allow_path(emb_runtime *rt, const char *dir)
{
    if (rt == nullptr || dir == nullptr || rt->ndirs >= EMBI_RT_MAX_DIRS) {
        return false;
    }
    size_t n = strlen(dir);
    if (n == 0 || n >= EMBI_RT_PATH_MAX) {
        return false;
    }
    char *d = SDL_malloc(n + 1);
    if (d == nullptr) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        d[i] = dir[i] == '\\' ? '/' : dir[i];
    }
    d[n] = '\0';
    while (n > 1 && d[n - 1] == '/') {
        d[--n] = '\0';
    }
    bool drive = n >= 2 && d[1] == ':';
    if (n == 0 || (d[0] != '/' && !drive)) {
        SDL_free(d);
        return false;
    }
    rt->dirs[rt->ndirs++] = d;
    return true;
}

/* Whether path (forward slashes) lies under an allowed directory. The root "/" allows every absolute path. A drive
 * directory ("C:") is compared without regard to case, as Windows paths are. Symbolic links are not resolved: a link
 * inside an allowed directory that points elsewhere is read (the embedder allows directories, not files). */
static bool path_allowed(const emb_runtime *rt, const char *path, size_t n)
{
    for (uint32_t i = 0; i < rt->ndirs; i++) {
        const char *d = rt->dirs[i];
        size_t dl = strlen(d);
        if (dl == 1 && d[0] == '/') {
            /* The root allows every absolute path: a POSIX one, or one with a drive letter. */
            if (n > 0 && (path[0] == '/' || (n >= 2 && path[1] == ':'))) {
                return true;
            }
            continue;
        }
        bool drive = dl >= 2 && d[1] == ':';
        if (n <= dl || path[dl] != '/') {
            continue;
        }
        int same = drive ? SDL_strncasecmp(path, d, dl) : SDL_strncmp(path, d, dl);
        if (same == 0) {
            return true;
        }
    }
    return false;
}

/* Reads the file of a file: URL or a bare path. 0: refused (a bad URL or a path outside the allowed directories),
 * 1: read, 2: allowed but not readable (missing). The bytes are SDL_malloc'd. */
int embi_rt_file_load(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len)
{
    char path[EMBI_RT_PATH_MAX];
    size_t plen = 0;
    if (!embi_url_to_path(url, path, sizeof path, &plen)) {
        embi_rt_log(rt, EMB_LOG_WARN, "file: URL refused: %.*s", (int)url.len, url.ptr);
        return 0;
    }
    if (!path_allowed(rt, path, plen)) {
        embi_rt_log(rt, EMB_LOG_WARN, "file: path not allowed: %s", path);
        return 0;
    }
    size_t n = 0;
    void *buf = SDL_LoadFile(path, &n);
    if (buf == nullptr) {
        embi_rt_log(rt, EMB_LOG_WARN, "file: cannot read %s", path);
        return 2;
    }
    *data = buf;
    *len = n;
    return 1;
}

static bool fetch_file(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len)
{
    return embi_rt_file_load(rt, url, data, len) == 1;
}

static bool fetch_handler(emb_runtime *rt, embi_str url, embi_str scheme, uint8_t **data, size_t *len)
{
    for (uint32_t i = 0; i < EMBI_RT_MAX_HANDLERS; i++) {
        embi_rt_handler *h = &rt->handlers[i];
        if (h->scheme == nullptr || !embi_url_scheme_is(scheme, h->scheme)) {
            continue;
        }
        char *cstr = SDL_malloc(url.len + 1);
        if (cstr == nullptr) {
            return false;
        }
        SDL_memcpy(cstr, url.ptr, url.len);
        cstr[url.len] = '\0';
        emb_resource res = {sizeof(emb_resource), nullptr, 0};
        bool ok = h->fn(h->user, cstr, url.len, &res);
        uint8_t *copy = nullptr;
        if (ok && res.len > 0 && res.data == nullptr) {
            ok = false;
        }
        if (ok) {
            copy = SDL_malloc(res.len > 0 ? res.len : 1);
            if (copy == nullptr) {
                ok = false;
            } else if (res.len > 0) {
                SDL_memcpy(copy, res.data, res.len);
            }
        }
        SDL_free(cstr);
        if (!ok) {
            SDL_free(copy);
            return false;
        }
        *data = copy;
        *len = res.len;
        return true;
    }
    embi_rt_log(rt, EMB_LOG_WARN, "no resource handler for %.*s", (int)scheme.len, scheme.ptr);
    return false;
}

bool embi_rt_fetch(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len)
{
    if (url.len == 0) {
        return false;
    }
    embi_str scheme;
    if (!embi_url_scheme(url, &scheme)) {
        return fetch_file(rt, url, data, len);
    }
    if (embi_url_scheme_is(scheme, "data")) {
        return embi_url_data(url, data, len);
    }
    if (embi_url_scheme_is(scheme, "file")) {
        return fetch_file(rt, url, data, len);
    }
    if (embi_url_scheme_is(scheme, "blob")) {
        return embi_rt_blob_lookup(rt, url, data, len, nullptr);
    }
    if (embi_url_scheme_is(scheme, "local")) {
        return false; /* only embi_rt_fetch_font resolves local(NAME) */
    }
    return fetch_handler(rt, url, scheme, data, len);
}

bool embi_rt_fetch_font(emb_runtime *rt, embi_str url, uint8_t **data, size_t *len)
{
    embi_str scheme;
    if (url.len > 0 && embi_url_scheme(url, &scheme) && embi_url_scheme_is(scheme, "local")) {
        /* local(NAME) of @font-face (W4i): the installed face whose full or PostScript name is NAME. A miss returns false,
         * and the registration then tries the next source. */
        if (url.len <= scheme.len + 1) {
            return false;
        }
        embi_str name = {url.ptr + scheme.len + 1, url.len - scheme.len - 1};
        return embi_fonts_local(rt->fonts, name, data, len);
    }
    return embi_rt_fetch(rt, url, data, len);
}

/* ------------------------------------------------------------------ networking (N1, include/ember/ember_net.h) */

#ifdef EMBER_NET
bool emb_runtime_net_allow(emb_runtime *rt, const char *pattern)
{
    return rt != nullptr && pattern != nullptr && embi_net_allow_add(rt->net, pattern);
}

bool emb_runtime_net_add_root(emb_runtime *rt, const void *pem_or_der, size_t len)
{
    return rt != nullptr && embi_net_add_root(rt->net, pem_or_der, len);
}

void emb_runtime_net_use_bundled_roots(emb_runtime *rt, bool on)
{
    if (rt != nullptr) {
        embi_net_use_bundled_roots(rt->net, on);
    }
}

void emb_runtime_net_set_user_agent(emb_runtime *rt, const char *ua)
{
    if (rt != nullptr) {
        embi_net_set_user_agent(rt->net, ua);
    }
}
#else
/* EMBER_NET=OFF: no network code is built; the functions exist so embedders link either way. */
bool emb_runtime_net_allow(emb_runtime *rt, const char *pattern)
{
    (void)rt;
    (void)pattern;
    return false;
}

bool emb_runtime_net_add_root(emb_runtime *rt, const void *pem_or_der, size_t len)
{
    (void)rt;
    (void)pem_or_der;
    (void)len;
    return false;
}

void emb_runtime_net_use_bundled_roots(emb_runtime *rt, bool on)
{
    (void)rt;
    (void)on;
}

void emb_runtime_net_set_user_agent(emb_runtime *rt, const char *ua)
{
    (void)rt;
    (void)ua;
}
#endif
