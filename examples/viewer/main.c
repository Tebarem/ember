/* A windowed viewer: opens a window and shows the page at the given file path or URL until the window is closed.
 *
 *   viewer [--dark] [--width W] [--height H] [--allow DIR] [--net-allow PATTERN]... [--startup-trace] PATH_OR_URL
 *
 * PATH_OR_URL may be an http or https URL: the page and its images, stylesheets, fonts and scripts load over the network,
 * from the hosts that --net-allow names (repeatable; host, host:port, *.example.com or *; nothing is allowed by default, a
 * request to any other host fails and logs a warning).
 *
 * --startup-trace prints the time of each startup step (runtime, view with its window and GPU device, load, first frame)
 * on stderr and exits after the first frame.
 *
 * PATH_OR_URL is a file path (relative paths resolve against the current directory), a file: URL, or a URL of a scheme
 * the embedder has a handler for (this example registers none). A directory is served as app://local/, so a Vite
 * build's dist/ folder runs with its index.html as the page. The file: loader reads only the directories given with
 * --allow; by default the directory of the page is readable, so its images and stylesheets load. The window takes its
 * scale from the display, so there is no scale option. Desktop only: it uses a plain main(), so it is not built on
 * Android or iOS (see examples/CMakeLists.txt). On Windows it is a windowed program with no console: the log goes to
 * stderr (redirect it to read it), and the messages that stop it also show in a message box. */

#define SDL_MAIN_HANDLED
#include <ember/ember.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define VIEWER_MAX_SIZE 16384L
#define VIEWER_MAX_NET_ALLOW 32

static void print_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    static const char *const names[] = {"debug", "info", "warn", "error"};
    int idx = (int)level;
    if (idx < 0 || idx > (int)EMB_LOG_ERROR) {
        idx = (int)EMB_LOG_ERROR;
    }
    fprintf(stderr, "[ember %s] %s\n", names[idx], message);
}

/* A message that stops the viewer: on stderr, and on Windows (a windowed program, so no console shows stderr) also in a
 * message box. */
static void fail(SDL_PRINTF_FORMAT_STRING const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void fail(SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
{
    char text[2304];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", text);
#ifdef SDL_PLATFORM_WINDOWS
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ember viewer", text, nullptr);
#endif
}

static void usage(const char *prog)
{
    fail("usage: %s [--dark] [--width W] [--height H] [--allow DIR] [--net-allow PATTERN]... [--startup-trace] PATH_OR_URL", prog);
}

/* The file path of a file: URL or a bare path, with forward slashes; relative paths are made absolute. False for other
 * URLs, which have no directory. */
static bool file_path_of(const char *target, char *out, size_t cap)
{
    const char *p = target;
    if (SDL_strncasecmp(p, "file:", 5) == 0) {
        p += 5;
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            if (p[0] == '/' && isalpha((unsigned char)p[1]) && p[2] == ':') {
                p++;
            }
        }
        size_t n = 0;
        for (; *p != '\0' && n + 1 < cap; p++) {
            if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hex[3] = {p[1], p[2], '\0'};
                out[n++] = (char)strtol(hex, nullptr, 16);
                p += 2;
            } else {
                out[n++] = *p == '\\' ? '/' : *p;
            }
        }
        out[n] = '\0';
        return n > 0;
    }
    if (strstr(p, "://") != nullptr) {
        return false;
    }
    bool absolute = p[0] == '/' || p[0] == '\\' || (isalpha((unsigned char)p[0]) && p[1] == ':');
    char *cwd = absolute ? nullptr : SDL_GetCurrentDirectory();
    size_t cl = cwd != nullptr ? SDL_strlen(cwd) : 0;
    while (cl > 0 && (cwd[cl - 1] == '/' || cwd[cl - 1] == '\\')) {
        cl--;
    }
    int n = cwd != nullptr ? SDL_snprintf(out, cap, "%.*s/%s", (int)cl, cwd, p) : SDL_snprintf(out, cap, "%s", p);
    SDL_free(cwd);
    for (char *c = out; *c != '\0'; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    return n > 0 && (size_t)n < cap;
}

/* The directory of the path, or nothing when it has none. */
static void allow_dir_of(emb_runtime *rt, const char *target)
{
    char path[2048];
    if (!file_path_of(target, path, sizeof path)) {
        return;
    }
    char *slash = strrchr(path, '/');
    if (slash == nullptr) {
        return;
    }
    if (slash == path) {
        emb_runtime_allow_path(rt, "/");
        return;
    }
    *slash = '\0';
    if (!emb_runtime_allow_path(rt, path)) {
        fprintf(stderr, "viewer: cannot allow directory %s\n", path);
    }
}

/* A size option: a whole number of CSS px in 1..VIEWER_MAX_SIZE. False (with a message) for anything else. */
static bool parse_size(const char *text, const char *what, uint32_t *out)
{
    char *end = nullptr;
    long v = strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < 1 || v > VIEWER_MAX_SIZE) {
        fail("viewer: %s must be a whole number from 1 to %ld", what, VIEWER_MAX_SIZE);
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* --startup-trace: the time since main() of each startup step, on stderr, and an exit after the first frame. */
static uint64_t trace_t0;
static bool trace_on;

static double trace_ms(void)
{
    return (double)(SDL_GetPerformanceCounter() - trace_t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

static void trace(const char *step)
{
    if (trace_on) {
        fprintf(stderr, "startup %-14s %8.1f ms\n", step, trace_ms());
    }
}

int main(int argc, char **argv)
{
    trace_t0 = SDL_GetPerformanceCounter();
    emb_color_scheme scheme = EMB_SCHEME_LIGHT;
    uint32_t width = 1024, height = 768;
    const char *target = nullptr;
    const char *allow = nullptr;
    const char *net_allow[VIEWER_MAX_NET_ALLOW];
    int n_net_allow = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--startup-trace") == 0) {
            trace_on = true;
            /* Buffered: on Windows each write of a redirected stderr can take tens of ms, which would be timed too. */
            setvbuf(stderr, nullptr, _IOFBF, 1 << 16);
        } else if (strcmp(argv[i], "--dark") == 0) {
            scheme = EMB_SCHEME_DARK;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], "--width", &width)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], "--height", &height)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            allow = argv[++i];
        } else if (strcmp(argv[i], "--net-allow") == 0 && i + 1 < argc) {
            if (n_net_allow == VIEWER_MAX_NET_ALLOW) {
                fail("viewer: at most %d --net-allow patterns", VIEWER_MAX_NET_ALLOW);
                return 2;
            }
            net_allow[n_net_allow++] = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            usage(argv[0]);
            return 2;
        } else {
            target = argv[i];
        }
    }
    if (target == nullptr) {
        usage(argv[0]);
        return 2;
    }

    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = print_log;
    emb_runtime *rt = emb_runtime_create(&rd);
    if (rt == nullptr) {
        return 1;
    }
    trace("runtime");
    for (int i = 0; i < n_net_allow; i++) {
        if (!emb_runtime_net_allow(rt, net_allow[i])) {
            fprintf(stderr, "viewer: cannot use --net-allow %s (a malformed pattern, or this build has no network)\n",
                    net_allow[i]);
        }
    }

    /* A directory (a built app, such as a Vite build's dist/) is served as app://local/ and its index.html is the page. */
    SDL_PathInfo info;
    if (SDL_GetPathInfo(target, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        if (!emb_runtime_mount_dir(rt, "app", "local", target)) {
            fail("viewer: cannot serve directory %s", target);
            emb_runtime_destroy(rt);
            return 1;
        }
        target = "app://local/index.html";
    } else if (allow != nullptr) {
        char dir[2048];
        if (!file_path_of(allow, dir, sizeof dir) || !emb_runtime_allow_path(rt, dir)) {
            fprintf(stderr, "viewer: cannot allow directory %s\n", allow);
        }
    } else {
        allow_dir_of(rt, target);
    }

    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_WINDOWED;
    vd.width = width;
    vd.height = height;
    vd.title = "Ember viewer";
    vd.scheme = scheme;
    emb_view *view = emb_view_create(rt, &vd);
    if (view == nullptr) {
        emb_runtime_destroy(rt);
        return 1;
    }

    trace("view");

    int status = 0;
    if (!emb_view_load_url(view, target)) {
        fail("viewer: cannot load %s", target);
        status = 1;
    } else if (trace_on) {
        trace("load");
        if (!emb_view_pump(view)) {
            status = 1;
        }
        trace("first frame");
        emb_view_stats st;
        memset(&st, 0, sizeof st);
        st.size = sizeof st;
        emb_view_stats_get(view, &st);
        fprintf(stderr, "stats pipelines %.1f ms, parse %.1f ms, fonts %.1f ms, style %.1f ms, layout %.1f ms, gpu %.1f ms\n",
                st.gpu_pipelines_ms, st.parse_ms, st.fonts_ms, st.style_ms, st.layout_ms, st.gpu_ms);
    } else if (!emb_view_run(view)) {
        status = 1;
    }

    emb_view_destroy(view);
    trace("view destroyed");
    emb_runtime_destroy(rt);
    trace("runtime gone");
    return status;
}
