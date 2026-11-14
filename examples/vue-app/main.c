/*
 * Shipping a web app with Ember: a Vue 3 + TanStack Query notes app (web/) in one executable.
 *
 *   - The Vite build (web/dist) is baked into the binary at build time by ember_embed_assets (C23 #embed), and served
 *     from app://local/ by the resource handler below. Nothing is read from disk at run time and nothing listens on a
 *     port: Ember has no network stack, and every fetch of the page goes through this handler.
 *   - The same handler is the app's native API: app://local/api/... is answered here in C. The page calls it with
 *     plain fetch(), so TanStack Query's caching, refetching and invalidation work as on the web.
 *   - The notes are saved by the host in the user's preference folder (SDL_GetPrefPath), and localStorage is persisted
 *     with emb_runtime_set_storage.
 *
 * Usage:
 *   vue_app                 the shipped build (embedded assets)
 *   vue_app --dev web/dist  serve the files of a directory instead, for `npm run watch` while developing
 */
#include <ember/ember.h>

#include <SDL3/SDL.h>

#include "vue_app_assets.h"

#include <stdlib.h>
#include <string.h>

#define APP_ORG "Ember Examples"
#define APP_NAME "Notes"
#define ORIGIN "app://local"

/* ------------------------------------------------------------------ a growable byte buffer */

typedef struct buf {
    char *data;
    size_t len, cap;
} buf;

static void buf_add(buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap == 0 ? 256 : b->cap;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *d = SDL_realloc(b->data, cap);
        if (d == nullptr) {
            return;
        }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_str(buf *b, const char *s) { buf_add(b, s, strlen(s)); }

/* Appends s as a JSON string literal. */
static void buf_json(buf *b, const char *s, size_t n)
{
    buf_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[8];
        if (c == '"' || c == '\\') {
            esc[0] = '\\';
            esc[1] = (char)c;
            buf_add(b, esc, 2);
        } else if (c < 0x20) {
            SDL_snprintf(esc, sizeof esc, "\\u%04x", c);
            buf_add(b, esc, 6);
        } else {
            buf_add(b, (const char *)&s[i], 1);
        }
    }
    buf_add(b, "\"", 1);
}

/* ------------------------------------------------------------------ the notes the host keeps */

typedef struct note {
    uint32_t id;
    char *text;
} note;

typedef struct app {
    const char *dev_dir; /* --dev: serve this directory instead of the embedded build */
    char *notes_path;    /* <pref path>/notes.txt */
    note *notes;
    size_t count, cap;
    uint32_t next_id;
    buf out; /* the body of the last API response (the library copies it) */
} app;

static void notes_add(app *a, const char *text, size_t n)
{
    if (a->count == a->cap) {
        size_t cap = a->cap == 0 ? 16 : a->cap * 2;
        note *d = SDL_realloc(a->notes, cap * sizeof *d);
        if (d == nullptr) {
            return;
        }
        a->notes = d;
        a->cap = cap;
    }
    char *t = SDL_malloc(n + 1);
    if (t == nullptr) {
        return;
    }
    memcpy(t, text, n);
    t[n] = '\0';
    a->notes[a->count++] = (note){a->next_id++, t};
}

/* The file holds one note per line; a newline inside a note is written as "\n" and a backslash as "\\". */
static void notes_save(const app *a)
{
    buf b = {};
    for (size_t i = 0; i < a->count; i++) {
        for (const char *p = a->notes[i].text; *p != '\0'; p++) {
            if (*p == '\n') {
                buf_add(&b, "\\n", 2);
            } else if (*p == '\\') {
                buf_add(&b, "\\\\", 2);
            } else if (*p != '\r') {
                buf_add(&b, p, 1);
            }
        }
        buf_add(&b, "\n", 1);
    }
    if (!SDL_SaveFile(a->notes_path, b.data != nullptr ? b.data : "", b.len)) {
        SDL_Log("notes: cannot save %s: %s", a->notes_path, SDL_GetError());
    }
    SDL_free(b.data);
}

static void notes_load(app *a)
{
    size_t len = 0;
    char *data = SDL_LoadFile(a->notes_path, &len);
    if (data == nullptr) {
        return; /* first run */
    }
    buf line = {};
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (line.len > 0) {
                notes_add(a, line.data, line.len);
            }
            line.len = 0;
        } else if (data[i] == '\r') {
            /* a line ending of a file edited on Windows */
        } else if (data[i] == '\\' && i + 1 < len) {
            i++;
            buf_add(&line, data[i] == 'n' ? "\n" : &data[i], 1);
        } else {
            buf_add(&line, &data[i], 1);
        }
    }
    SDL_free(line.data);
    SDL_free(data);
}

/* ------------------------------------------------------------------ app://local/ */

static const char *mime_of(const char *path)
{
    static const struct {
        const char *ext, *mime;
    } types[] = {
        {".html", "text/html"},       {".js", "text/javascript"}, {".mjs", "text/javascript"},
        {".css", "text/css"},         {".json", "application/json"}, {".svg", "image/svg+xml"},
        {".png", "image/png"},        {".jpg", "image/jpeg"},     {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},        {".ico", "image/x-icon"},   {".woff2", "font/woff2"},
        {".woff", "font/woff"},       {".ttf", "font/ttf"},       {".otf", "font/otf"},
        {".txt", "text/plain"},
    };
    const char *dot = strrchr(path, '.');
    for (size_t i = 0; dot != nullptr && i < SDL_arraysize(types); i++) {
        if (SDL_strcasecmp(dot, types[i].ext) == 0) {
            return types[i].mime;
        }
    }
    return "application/octet-stream";
}

static int asset_cmp(const void *key, const void *elem)
{
    return strcmp((const char *)key, ((const ember_asset *)elem)->path);
}

static void respond(emb_response *out, uint16_t status, const char *mime, const void *data, size_t len)
{
    out->status = status;
    out->mime = mime;
    out->mime_len = mime != nullptr ? strlen(mime) : 0;
    out->data = data;
    out->len = len;
}

static void respond_json(app *a, emb_response *out, uint16_t status)
{
    respond(out, status, "application/json", a->out.data, a->out.len);
}

/* The native API: GET api/system, GET api/notes, POST api/notes (the body is the text), DELETE api/notes/<id>. */
static void serve_api(app *a, const char *method, const char *route, const emb_request_info *req, emb_response *out)
{
    a->out.len = 0;
    if (strcmp(route, "system") == 0 && strcmp(method, "GET") == 0) {
        char line[512];
        SDL_snprintf(line, sizeof line, "{\"platform\":\"%s\",\"cpus\":%d,\"ramMB\":%d,\"ember\":\"%s\",\"notesFile\":",
                     SDL_GetPlatform(), SDL_GetNumLogicalCPUCores(), SDL_GetSystemRAM(), emb_version_string());
        buf_str(&a->out, line);
        buf_json(&a->out, a->notes_path, strlen(a->notes_path));
        buf_str(&a->out, "}");
        respond_json(a, out, 200);
        return;
    }
    if (strcmp(route, "notes") == 0 && strcmp(method, "GET") == 0) {
        buf_str(&a->out, "[");
        for (size_t i = 0; i < a->count; i++) {
            char id[48];
            SDL_snprintf(id, sizeof id, "%s{\"id\":%u,\"text\":", i > 0 ? "," : "", (unsigned)a->notes[i].id);
            buf_str(&a->out, id);
            buf_json(&a->out, a->notes[i].text, strlen(a->notes[i].text));
            buf_str(&a->out, "}");
        }
        buf_str(&a->out, "]");
        respond_json(a, out, 200);
        return;
    }
    if (strcmp(route, "notes") == 0 && strcmp(method, "POST") == 0) {
        if (req->body == nullptr || req->body_len == 0 || req->body_len > 4096) {
            buf_str(&a->out, "{\"error\":\"a note is 1 to 4096 bytes\"}");
            respond_json(a, out, 400);
            return;
        }
        notes_add(a, (const char *)req->body, req->body_len);
        notes_save(a);
        buf_str(&a->out, "{}");
        respond_json(a, out, 201);
        return;
    }
    if (strncmp(route, "notes/", 6) == 0 && strcmp(method, "DELETE") == 0) {
        uint32_t id = (uint32_t)SDL_strtoul(route + 6, nullptr, 10);
        for (size_t i = 0; i < a->count; i++) {
            if (a->notes[i].id == id) {
                SDL_free(a->notes[i].text);
                memmove(&a->notes[i], &a->notes[i + 1], (a->count - i - 1) * sizeof *a->notes);
                a->count--;
                notes_save(a);
                respond(out, 204, nullptr, nullptr, 0);
                return;
            }
        }
    }
    buf_str(&a->out, "{\"error\":\"no such API call\"}");
    respond_json(a, out, 404);
}

static bool serve(void *user, const emb_request_info *req, emb_response *out)
{
    app *a = user;
    /* The path of the URL, without the origin, the query and the fragment. */
    char path[1024];
    size_t origin = strlen(ORIGIN);
    if (req->url_len <= origin || SDL_strncasecmp(req->url, ORIGIN, origin) != 0 || req->url[origin] != '/') {
        return false; /* another host of the scheme */
    }
    size_t n = 0;
    for (size_t i = origin; i < req->url_len && req->url[i] != '?' && req->url[i] != '#'; i++) {
        if (n + 1 >= sizeof path) {
            return false;
        }
        path[n++] = req->url[i];
    }
    path[n] = '\0';
    char method[16];
    SDL_snprintf(method, sizeof method, "%.*s", (int)req->method_len, req->method);

    if (strncmp(path, "/api/", 5) == 0) {
        serve_api(a, method, path + 5, req, out);
        return true;
    }
    if (strcmp(path, "/") == 0) {
        SDL_strlcpy(path, "/index.html", sizeof path);
    }
    if (strstr(path, "..") != nullptr || strchr(path, '\\') != nullptr || strchr(path, ':') != nullptr) {
        return false;
    }

    if (a->dev_dir != nullptr) {
        /* Development: read the file from the directory on every request, so a rebuild shows on reload. */
        char *file = nullptr;
        SDL_asprintf(&file, "%s%s", a->dev_dir, path);
        size_t len = 0;
        void *data = file != nullptr ? SDL_LoadFile(file, &len) : nullptr;
        SDL_free(file);
        if (data == nullptr) {
            respond(out, 404, "text/plain", "not found", 9);
            return true;
        }
        respond(out, 200, mime_of(path), data, len);
        SDL_free(a->out.data); /* the library copies the body before this returns; free it on the next call */
        a->out = (buf){data, len, len};
        return true;
    }
    const ember_asset *f = bsearch(path, vue_app_assets, vue_app_assets_count, sizeof *vue_app_assets, asset_cmp);
    if (f == nullptr) {
        respond(out, 404, "text/plain", "not found", 9);
        return true;
    }
    respond(out, 200, mime_of(path), f->data, f->size);
    return true;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    app a = {.next_id = 1};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc) {
            a.dev_dir = argv[++i];
        }
    }

    char *pref = SDL_GetPrefPath(APP_ORG, APP_NAME);
    if (pref == nullptr) {
        SDL_Log("no preference folder: %s", SDL_GetError());
        return 1;
    }
    SDL_asprintf(&a.notes_path, "%snotes.txt", pref);
    SDL_free(pref);
    notes_load(&a);

    emb_runtime_desc rd = {.size = sizeof rd};
    emb_runtime *rt = emb_runtime_create(&rd);
    if (rt == nullptr) {
        return 1;
    }
    emb_runtime_set_storage(rt, APP_ORG, APP_NAME); /* localStorage survives restarts */
    emb_runtime_set_resource_handler_v2(rt, "app", serve, &a);

    emb_view_desc vd = {
        .size = sizeof vd,
        .mode = EMB_VIEW_WINDOWED,
        .width = 640,
        .height = 720,
        .title = APP_NAME,
    };
    emb_view *view = emb_view_create(rt, &vd);
    if (view != nullptr && SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK) {
        emb_view_set_color_scheme(view, EMB_SCHEME_DARK); /* the view has opened SDL video, so the theme is known */
    }
    int rc = 1;
    if (view != nullptr && emb_view_load_url(view, ORIGIN "/index.html")) {
        rc = emb_view_run(view) ? 0 : 1;
    }

    if (view != nullptr) {
        emb_view_destroy(view);
    }
    emb_runtime_destroy(rt);
    for (size_t i = 0; i < a.count; i++) {
        SDL_free(a.notes[i].text);
    }
    SDL_free(a.notes);
    SDL_free(a.notes_path);
    SDL_free(a.out.data);
    return rc;
}
