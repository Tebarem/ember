#pragma once
/*
 * Scenario player of the framework apps (W8b0). It is included at the end of
 * test_embed_apps.c, after the canary, and uses the canary's helpers (render_now, print_message, blank_items, k_apps).
 *
 * It runs tests/apps/<name>/scenario.json against app://local/ (the app's dist/ folder, mounted with
 * emb_runtime_mount_dir) in an offscreen view. Input goes through the W5d input API (emb_view_mouse_*, emb_view_key,
 * emb_view_text_input) at the centre of the target element, found with getBoundingClientRect; the player also asks
 * document.elementFromPoint what is under that point, so a click that a real user could not make (the target is
 * covered) fails its step. The page is read only through emb_view_eval, for the expectations.
 *
 * scenario.json: {"app": name, "viewport": [w, h], "steps": [step, ...]}. A step is an object with "op" and these
 * optional fields: "sel" (CSS selector), "nth" (index among the matches, default 0), "text", "js", "name", "key",
 * "code", "mods" ("ctrl+shift"), "n" (a count), "ms" (a wait or poll limit), "flag" (false for the negated form of
 * expect-class and expect-checked), "mode" ("contains" for expect-text, expect-path and expect-eval), "boot" (on the
 * first wait-for: when it fails the app did not start and the remaining steps are reported as blocked).
 * Ops: wait-for, wait, click, dblclick, hover, type (click, then text), text (text input to the focused element), key,
 * navigate (sets location.hash), eval, reload (destroy the view, create a new one, load the same URL again; the runtime
 * and its localStorage stay), expect-text, expect-count, expect-class, expect-value, expect-checked, expect-hash,
 * expect-path, expect-storage, expect-focus, expect-eval, expect-paint (the frame has at least "n" more display list
 * items than a blank page), expect-gold (W8b: the box of "sel", or the whole view without one, read back from the view's
 * texture, halved by 2x2 averaging and compared with the golden image "name" of tests/gold through emb_gold.h, whose
 * tolerances absorb antialiasing differences between GPU drivers; EMBER_UPDATE_GOLDENS=1 writes it). The five TodoMVC
 * apps share their goldens: the same markup and stylesheet must paint the same pixels whatever framework made the DOM.
 * The runtime of the player uses the bundled fonts only (EMB_RUNTIME_NO_SYSTEM_FONTS), so the pixels do not depend on
 * the machine's fonts.
 * Every expect polls (pumping frames) for up to its "ms" (default 300 ms in strict mode, 100 ms in the report mode,
 * EMBER_APPS_SETTLE_MS overrides, three times longer under AddressSanitizer) before it fails. A failing step is recorded and
 * the player goes on with the next one.
 *
 * Strict mode (the default since W8b; EMBER_APPS_STRICT=0 turns it off) makes every step of every scenario, and every
 * library error line, a test failure.
 * EMBER_APPS_ONLY=<app> runs one app.
 */
#include <stdarg.h>

#include "emb_gold.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#define SC_STR 256
#define SC_MAX_STEPS 160

typedef struct sc_step {
    char op[24];
    char sel[SC_STR];
    char text[SC_STR];
    char js[SC_STR * 2];
    char name[64];
    char key[24];
    char code[24];
    char mods[32];
    char mode[16];
    int nth;
    int n;
    int ms;
    int flag; /* default 1 */
    bool boot;
} sc_step;

typedef struct scenario {
    char app[64];
    int vw;
    int vh;
    int count;
    sc_step steps[SC_MAX_STEPS];
} scenario;

/* ---- A small JSON reader: objects of scalars for the steps, anything else is skipped. ---- */
static const char *js_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static void put_utf8(char *out, size_t cap, size_t *len, uint32_t cp)
{
    char tmp[4];
    int n = 0;
    if (cp < 0x80) {
        tmp[n++] = (char)cp;
    } else if (cp < 0x800) {
        tmp[n++] = (char)(0xC0 | (cp >> 6));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp >= 0x10000) {
        tmp[n++] = (char)(0xF0 | (cp >> 18));
        tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        tmp[n++] = (char)(0xE0 | (cp >> 12));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    }
    for (int i = 0; i < n; i++) {
        if (*len + 1 < cap) {
            out[(*len)++] = tmp[i];
        }
    }
}

/* Parses the string at p (the opening quote); returns the position after it, or nullptr. */
static const char *js_string(const char *p, char *out, size_t cap)
{
    if (*p != '"') {
        return nullptr;
    }
    p++;
    size_t len = 0;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == '\0') {
                return nullptr;
            }
            if (*p == 'u') {
                uint32_t cp = 0;
                for (int i = 1; i <= 4; i++) {
                    char c = p[i];
                    if (!SDL_isxdigit((unsigned char)c)) {
                        return nullptr;
                    }
                    cp = cp * 16 + (uint32_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
                }
                p += 4;
                /* a high surrogate followed by a \uDC00-\uDFFF escape is one supplementary code point */
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                    uint32_t lo = 0;
                    for (int i = 3; i <= 6; i++) {
                        char c = p[i];
                        if (!SDL_isxdigit((unsigned char)c)) {
                            return nullptr;
                        }
                        lo = lo * 16 + (uint32_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
                    }
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                put_utf8(out, cap, &len, cp);
            } else {
                char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                if (len + 1 < cap) {
                    out[len++] = c;
                }
            }
            p++;
        } else {
            if (len + 1 < cap) {
                out[len++] = *p;
            }
            p++;
        }
    }
    if (*p != '"') {
        return nullptr;
    }
    if (cap > 0) {
        out[len] = '\0';
    }
    return p + 1;
}

static const char *js_skip(const char *p)
{
    p = js_ws(p);
    char scratch[8];
    if (*p == '"') {
        return js_string(p, scratch, sizeof scratch);
    }
    if (*p == '{' || *p == '[') {
        const char close = *p == '{' ? '}' : ']';
        p = js_ws(p + 1);
        while (p != nullptr && *p != '\0' && *p != close) {
            if (close == '}') {
                p = js_string(p, scratch, sizeof scratch);
                if (p == nullptr) {
                    return nullptr;
                }
                p = js_ws(p);
                if (*p != ':') {
                    return nullptr;
                }
                p++;
            }
            p = js_skip(p);
            if (p == nullptr) {
                return nullptr;
            }
            p = js_ws(p);
            if (*p == ',') {
                p = js_ws(p + 1);
            }
        }
        return (p != nullptr && *p == close) ? p + 1 : nullptr;
    }
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
        p++;
    }
    return p;
}

static void step_set(sc_step *s, const char *key, const char *str, double num)
{
    if (SDL_strcmp(key, "op") == 0) SDL_strlcpy(s->op, str, sizeof s->op);
    else if (SDL_strcmp(key, "sel") == 0) SDL_strlcpy(s->sel, str, sizeof s->sel);
    else if (SDL_strcmp(key, "text") == 0) SDL_strlcpy(s->text, str, sizeof s->text);
    else if (SDL_strcmp(key, "js") == 0) SDL_strlcpy(s->js, str, sizeof s->js);
    else if (SDL_strcmp(key, "name") == 0) SDL_strlcpy(s->name, str, sizeof s->name);
    else if (SDL_strcmp(key, "key") == 0) SDL_strlcpy(s->key, str, sizeof s->key);
    else if (SDL_strcmp(key, "code") == 0) SDL_strlcpy(s->code, str, sizeof s->code);
    else if (SDL_strcmp(key, "mods") == 0) SDL_strlcpy(s->mods, str, sizeof s->mods);
    else if (SDL_strcmp(key, "mode") == 0) SDL_strlcpy(s->mode, str, sizeof s->mode);
    else if (SDL_strcmp(key, "nth") == 0) s->nth = (int)num;
    else if (SDL_strcmp(key, "n") == 0) s->n = (int)num;
    else if (SDL_strcmp(key, "ms") == 0) s->ms = (int)num;
    else if (SDL_strcmp(key, "flag") == 0) s->flag = num != 0;
    else if (SDL_strcmp(key, "boot") == 0) s->boot = num != 0;
}

/* Parses one step object at p ('{'). */
static const char *js_step(const char *p, sc_step *s)
{
    SDL_memset(s, 0, sizeof *s);
    s->flag = 1;
    p = js_ws(p + 1);
    while (*p != '\0' && *p != '}') {
        char key[32];
        p = js_string(p, key, sizeof key);
        if (p == nullptr) {
            return nullptr;
        }
        p = js_ws(p);
        if (*p != ':') {
            return nullptr;
        }
        p = js_ws(p + 1);
        char str[SC_STR * 2];
        str[0] = '\0';
        double num = 0;
        if (*p == '"') {
            p = js_string(p, str, sizeof str);
            if (p == nullptr) {
                return nullptr;
            }
        } else if (SDL_strncmp(p, "true", 4) == 0) {
            num = 1;
            p += 4;
        } else if (SDL_strncmp(p, "false", 5) == 0) {
            num = 0;
            p += 5;
        } else {
            char *end = nullptr;
            num = SDL_strtod(p, &end);
            if (end == p) {
                return nullptr;
            }
            p = end;
        }
        step_set(s, key, str, num);
        p = js_ws(p);
        if (*p == ',') {
            p = js_ws(p + 1);
        }
    }
    return *p == '}' ? p + 1 : nullptr;
}

/* Loads tests/apps/<app>/scenario.json into sc. Returns false with a reason. */
static bool scenario_load(const char *app, scenario *sc, const char **why)
{
    char path[1280];
    SDL_snprintf(path, sizeof path, "%s/tests/apps/%s/scenario.json", EMBER_SOURCE_DIR, app);
    const char *over = SDL_getenv("EMBER_APPS_SCENARIO"); /* a debugging aid: another scenario file for the run */
    if (over != nullptr) {
        SDL_snprintf(path, sizeof path, "%s", over);
    }
    size_t size = 0;
    char *data = SDL_LoadFile(path, &size);
    if (data == nullptr) {
        *why = "scenario.json missing";
        return false;
    }
    SDL_memset(sc, 0, sizeof *sc);
    sc->vw = 800;
    sc->vh = 600;
    SDL_strlcpy(sc->app, app, sizeof sc->app);
    const char *p = js_ws(data);
    bool ok = *p == '{';
    if (ok) {
        p = js_ws(p + 1);
    }
    while (ok && *p != '\0' && *p != '}') {
        char key[32];
        p = js_string(p, key, sizeof key);
        p = p != nullptr ? js_ws(p) : nullptr;
        if (p == nullptr || *p != ':') {
            ok = false;
            break;
        }
        p = js_ws(p + 1);
        if (SDL_strcmp(key, "viewport") == 0 && *p == '[') {
            char *e = nullptr;
            sc->vw = (int)SDL_strtod(js_ws(p + 1), &e);
            e = (char *)js_ws(e);
            if (*e == ',') {
                sc->vh = (int)SDL_strtod(js_ws(e + 1), &e);
            }
            p = js_skip(p);
        } else if (SDL_strcmp(key, "steps") == 0 && *p == '[') {
            p = js_ws(p + 1);
            while (p != nullptr && *p == '{' && sc->count < SC_MAX_STEPS) {
                p = js_step(p, &sc->steps[sc->count++]);
                p = p != nullptr ? js_ws(p) : nullptr;
                if (p != nullptr && *p == ',') {
                    p = js_ws(p + 1);
                }
            }
            if (p == nullptr || *p != ']') {
                ok = false;
                break;
            }
            p++;
        } else {
            p = js_skip(p);
        }
        p = p != nullptr ? js_ws(p) : nullptr;
        if (p == nullptr) {
            ok = false;
            break;
        }
        if (*p == ',') {
            p = js_ws(p + 1);
        }
    }
    SDL_free(data);
    if (!ok) {
        *why = "scenario.json is not valid";
        return false;
    }
    return true;
}

/* The resident set of this process in KB from /proc/self/statm, or -1 where there is none (Windows, macOS). */
static long rss_kb(void)
{
    SDL_IOStream *io = SDL_IOFromFile("/proc/self/statm", "r");
    if (io == nullptr) {
        return -1;
    }
    char buf[128];
    size_t n = SDL_ReadIO(io, buf, sizeof buf - 1);
    SDL_CloseIO(io);
    buf[n] = '\0';
    char *end = nullptr;
    (void)SDL_strtol(buf, &end, 10);
    long pages = SDL_strtol(end, nullptr, 10);
    return pages * 4; /* approximate: assumes 4 KB pages (x86-64 and arm64 Linux hosts of the box); informational only */
}

/* ---- The player. ---- */
#define ERR_KEEP 6

typedef struct play {
    SDL_GPUDevice *dev;
    emb_runtime *rt;
    emb_view *v;
    uint32_t blank;
    int vw;
    int vh;
    int settle_ms;
    int step_index;
    char url[1024];
    int errors;
    int warnings;
    int err_step[ERR_KEEP];
    char err_text[ERR_KEEP][1500];
} play;

static play *g_play;

static void play_on_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    play *p = g_play;
    if (p == nullptr) {
        return;
    }
    if (level == EMB_LOG_ERROR) {
        if (p->errors < ERR_KEEP) {
            p->err_step[p->errors] = p->step_index;
            SDL_snprintf(p->err_text[p->errors], sizeof p->err_text[0], "%s", message);
        }
        p->errors++;
    } else if (level == EMB_LOG_WARN) {
        p->warnings++;
    }
}

/* Renders frames for ms milliseconds (at least one), so timers, microtasks, fetches and animations run. */
static bool pump(play *p, int ms)
{
    bool changed = false;
    const uint64_t t0 = SDL_GetTicksNS();
    const uint64_t budget = (uint64_t)ms * 1000000u;
    for (;;) {
        if (render_now(p->dev, p->v)) {
            changed = true;
        }
        if (SDL_GetTicksNS() - t0 >= budget) {
            break;
        }
        SDL_Delay(1);
    }
    return changed;
}

static char g_ev[4096];
static bool g_ev_ok;

static void ev_cb(void *user, bool ok, const char *text, size_t len)
{
    (void)user;
    g_ev_ok = ok;
    size_t n = len < sizeof g_ev - 1 ? len : sizeof g_ev - 1;
    SDL_memcpy(g_ev, text, n);
    g_ev[n] = '\0';
}

/* Runs js; the result text (or the exception text) is left in g_ev. */
static bool eval_js(play *p, const char *js)
{
    g_ev[0] = '\0';
    g_ev_ok = false;
    (void)emb_view_eval(p->v, js, SDL_strlen(js), ev_cb, nullptr);
    return g_ev_ok;
}

/* Appends s to dst as a JS string literal. */
static void quote_js(char *dst, size_t cap, const char *s)
{
    size_t n = SDL_strlen(dst);
    if (n + 2 >= cap) {
        return;
    }
    dst[n++] = '"';
    for (; *s != '\0' && n + 3 < cap; s++) {
        if (*s == '"' || *s == '\\') {
            dst[n++] = '\\';
            dst[n++] = *s;
        } else if (*s == '\n') {
            dst[n++] = '\\';
            dst[n++] = 'n';
        } else {
            dst[n++] = *s;
        }
    }
    dst[n++] = '"';
    dst[n] = '\0';
}

/* "document.querySelectorAll(sel)[nth]" */
static void elem_expr(char *dst, size_t cap, const sc_step *s)
{
    dst[0] = '\0';
    SDL_strlcat(dst, "document.querySelectorAll(", cap);
    quote_js(dst, cap, s->sel);
    char tail[32];
    SDL_snprintf(tail, sizeof tail, ")[%d]", s->nth);
    SDL_strlcat(dst, tail, cap);
}

typedef struct step_result {
    bool ok;
    char detail[400];
} step_result;

static void fail(step_result *r, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
static void fail(step_result *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(r->detail, sizeof r->detail, fmt, ap);
    va_end(ap);
    r->ok = false;
}

/* Finds the centre of the step's element and what is under it. Returns false when the element is missing or its
 * geometry cannot be read (r says why). *hit_ok is false (and r failed) when another element covers the centre; the
 * coordinates are still set then, so the caller can make the click a user would have made. */
static bool locate(play *p, const sc_step *s, float *x, float *y, bool *hit_ok, step_result *r)
{
    char e[SC_STR * 2];
    elem_expr(e, sizeof e, s);
    char js[1600];
    SDL_snprintf(js, sizeof js,
                 "(function(){var e=%s;if(!e)return 'none';var r=e.getBoundingClientRect();"
                 "var x=r.left+r.width/2,y=r.top+r.height/2,h='ok';"
                 "try{var t=document.elementFromPoint(x,y);"
                 "if(!t)h='nothing';"
                 "else if(!(t===e||e.contains(t)||(t.closest&&t.closest('label')&&t.closest('label').control===e)))"
                 "h=t.tagName+'.'+t.className;}catch(err){h='?'+err}"
                 "return x+','+y+','+r.width+','+r.height+','+h;})()",
                 e);
    (void)pump(p, 0); /* lay out pending changes first */
    *hit_ok = true;
    if (!eval_js(p, js)) {
        fail(r, "cannot read geometry of '%s'[%d]: %.200s", s->sel, s->nth, g_ev);
        return false;
    }
    if (SDL_strcmp(g_ev, "none") == 0) {
        fail(r, "no element matches '%s'[%d]", s->sel, s->nth);
        return false;
    }
    char *end = nullptr;
    *x = (float)SDL_strtod(g_ev, &end);
    *y = (float)SDL_strtod(end + 1, &end);
    const double w = SDL_strtod(end + 1, &end);
    const double h = SDL_strtod(end + 1, &end);
    const char *hit = end + 1;
    if (!(w > 0) || !(h > 0)) {
        fail(r, "'%s'[%d] has an empty box (%gx%g)", s->sel, s->nth, w, h);
        return false;
    }
    if (SDL_strcmp(hit, "ok") != 0 && hit[0] != '?') {
        *hit_ok = false;
        fail(r, "the point (%.0f,%.0f) of '%s'[%d] is covered by %.100s", *x, *y, s->sel, s->nth, hit);
    }
    return true;
}

static void click_at(play *p, float x, float y, int clicks)
{
    emb_view_mouse_move(p->v, x, y, 0);
    for (int i = 0; i < clicks; i++) {
        emb_view_mouse_button(p->v, x, y, EMB_BUTTON_PRIMARY, true, 0);
        emb_view_mouse_button(p->v, x, y, EMB_BUTTON_PRIMARY, false, 0);
    }
    (void)pump(p, 30);
}

static uint32_t parse_mods(const char *m)
{
    uint32_t r = 0;
    if (SDL_strstr(m, "ctrl")) r |= EMB_MOD_CTRL;
    if (SDL_strstr(m, "shift")) r |= EMB_MOD_SHIFT;
    if (SDL_strstr(m, "alt")) r |= EMB_MOD_ALT;
    if (SDL_strstr(m, "meta")) r |= EMB_MOD_META;
    return r;
}

/* Polls an expression until its text equals (or contains) want, for up to ms. */
static void expect_js(play *p, const char *js, const char *want, bool contains, int ms, step_result *r)
{
    const uint64_t t0 = SDL_GetTicksNS();
    for (;;) {
        const bool ok = eval_js(p, js);
        const bool match = ok && (contains ? SDL_strstr(g_ev, want) != nullptr : SDL_strcmp(g_ev, want) == 0);
        if (match) {
            return;
        }
        if ((SDL_GetTicksNS() - t0) / 1000000u >= (uint64_t)ms) {
            fail(r, "wanted%s '%.100s'; %s '%.150s'", contains ? " to contain" : "", want, ok ? "got" : "threw", g_ev);
            return;
        }
        (void)pump(p, 15);
    }
}

static bool view_load(play *p, const char *url)
{
    emb_view_desc vd;
    SDL_memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = (uint32_t)p->vw;
    vd.height = (uint32_t)p->vh;
    vd.scale = 1.0f;
    vd.device = p->dev;
    p->v = emb_view_create(p->rt, &vd);
    return p->v != nullptr && emb_view_load_url(p->v, url);
}

/* Reads the view's texture back as tightly packed RGBA8 (w x h, SDL_free), or nullptr. */
static uint8_t *readback_view(SDL_GPUDevice *dev, emb_view *v, uint32_t w, uint32_t h)
{
    const size_t bytes = (size_t)w * h * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    SDL_memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    SDL_GPUCommandBuffer *cmd = tb != nullptr ? SDL_AcquireGPUCommandBuffer(dev) : nullptr;
    if (cmd == nullptr) {
        if (tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(dev, tb);
        }
        return nullptr;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    SDL_memset(&region, 0, sizeof region);
    region.texture = emb_view_texture(v);
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    SDL_memset(&info, 0, sizeof info);
    info.transfer_buffer = tb;
    info.pixels_per_row = w;
    info.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    uint8_t *out = nullptr;
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
        if (mapped != nullptr) {
            out = SDL_malloc(bytes);
            if (out != nullptr) {
                SDL_memcpy(out, mapped, bytes);
            }
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return out;
}

/* expect-gold: the region (the box of s->sel, else the view) of the current frame, halved, against the golden s->name. */
static void expect_gold(play *p, const sc_step *s, step_result *r)
{
#if !EMBER_SVG
    /* SZ3b: the TodoMVC style sheet draws the check mark of a completed item as an SVG data URI, which a build without EMBER_SVG
     * does not decode, so the golden of a state with a completed item cannot match. */
    if (SDL_strcmp(s->name, "apps_todomvc_one_done") == 0) {
        return;
    }
#endif
    int x0 = 0, y0 = 0, x1 = p->vw, y1 = p->vh;
    if (s->sel[0] != '\0') {
        char e[SC_STR * 2];
        elem_expr(e, sizeof e, s);
        char js[1024];
        SDL_snprintf(js, sizeof js,
                     "(function(){var e=%s;if(!e)return 'none';var r=e.getBoundingClientRect();"
                     "return Math.floor(r.left)+','+Math.floor(r.top)+','+Math.ceil(r.right)+','+Math.ceil(r.bottom)})()",
                     e);
        if (!eval_js(p, js) || SDL_sscanf(g_ev, "%d,%d,%d,%d", &x0, &y0, &x1, &y1) != 4) {
            fail(r, "no element matches '%s'[%d] (%.100s)", s->sel, s->nth, g_ev);
            return;
        }
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > p->vw ? p->vw : x1;
        y1 = y1 > p->vh ? p->vh : y1;
    }
    const int w = (x1 - x0) / 2, h = (y1 - y0) / 2;
    if (w <= 0 || h <= 0) {
        fail(r, "the region of '%s' is empty", s->sel);
        return;
    }
    (void)pump(p, 0);
    uint8_t *full = readback_view(p->dev, p->v, (uint32_t)p->vw, (uint32_t)p->vh);
    uint8_t *half = SDL_malloc((size_t)w * (size_t)h * 4u);
    if (full == nullptr || half == nullptr) {
        SDL_free(full);
        SDL_free(half);
        fail(r, "cannot read the view's texture back");
        return;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 4; c++) {
                int sum = 0;
                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        sum += full[((size_t)(y0 + 2 * y + dy) * (size_t)p->vw + (size_t)(x0 + 2 * x + dx)) * 4u + (size_t)c];
                    }
                }
                half[((size_t)y * (size_t)w + (size_t)x) * 4u + (size_t)c] = (uint8_t)((sum + 2) / 4);
            }
        }
    }
    if (!emb_gold_check(s->name, half, w, h)) {
        fail(r, "the %dx%d region at (%d,%d), halved, does not match the golden '%s' (gold-out has the diff)", x1 - x0,
             y1 - y0, x0, y0, s->name);
    }
    SDL_free(full);
    SDL_free(half);
}

static void run_step(play *p, const sc_step *s, step_result *r)
{
    r->ok = true;
    r->detail[0] = '\0';
    const int ms = s->ms > 0 ? s->ms : p->settle_ms;
    char e[SC_STR * 2];
    elem_expr(e, sizeof e, s);
    char js[2048];
    const char *op = s->op;
    float x = 0, y = 0;
    bool hit = true;

    if (SDL_strcmp(op, "wait-for") == 0) {
        SDL_snprintf(js, sizeof js, "String(document.querySelectorAll(");
        quote_js(js, sizeof js, s->sel);
        SDL_strlcat(js, ").length>0)", sizeof js);
        expect_js(p, js, "true", false, s->ms > 0 ? s->ms : 2000, r);
    } else if (SDL_strcmp(op, "wait") == 0) {
        (void)pump(p, s->ms > 0 ? s->ms : 100);
    } else if (SDL_strcmp(op, "click") == 0 || SDL_strcmp(op, "dblclick") == 0 || SDL_strcmp(op, "type") == 0) {
        if (locate(p, s, &x, &y, &hit, r) || !hit) {
            /* a covered point is clicked all the same: that is what the user's click would have done */
            click_at(p, x, y, SDL_strcmp(op, "dblclick") == 0 ? 2 : 1);
            if (SDL_strcmp(op, "type") == 0) {
                emb_view_text_input(p->v, s->text, SDL_strlen(s->text));
                (void)pump(p, 30);
            }
        }
    } else if (SDL_strcmp(op, "hover") == 0) {
        if (locate(p, s, &x, &y, &hit, r) || !hit) {
            emb_view_mouse_move(p->v, x, y, 0);
            (void)pump(p, 30);
        }
    } else if (SDL_strcmp(op, "text") == 0) {
        emb_view_text_input(p->v, s->text, SDL_strlen(s->text));
        (void)pump(p, 30);
    } else if (SDL_strcmp(op, "key") == 0) {
        emb_key_event d;
        SDL_memset(&d, 0, sizeof d);
        d.size = sizeof d;
        d.down = true;
        d.mods = parse_mods(s->mods);
        d.key = s->key;
        d.code = s->code[0] ? s->code : s->key;
        emb_view_key(p->v, &d);
        d.down = false;
        emb_view_key(p->v, &d);
        (void)pump(p, 30);
    } else if (SDL_strcmp(op, "navigate") == 0) {
        SDL_snprintf(js, sizeof js, "location.hash = ");
        quote_js(js, sizeof js, s->text);
        if (!eval_js(p, js)) {
            fail(r, "threw: %.200s", g_ev);
        }
        (void)pump(p, 40);
    } else if (SDL_strcmp(op, "eval") == 0) {
        if (!eval_js(p, s->js)) {
            fail(r, "threw: %.200s", g_ev);
        }
        (void)pump(p, 40);
    } else if (SDL_strcmp(op, "time") == 0) {
        /* a diagnostic: runs js and logs its result and the wall time it took (always passes unless it throws) */
        const uint64_t tt = SDL_GetTicksNS();
        const bool ok = eval_js(p, s->js);
        SDL_Log("SCEN time: %.1f ms, %s '%.900s'", (double)(SDL_GetTicksNS() - tt) / 1e6, ok ? "result" : "threw", g_ev);
        if (!ok) {
            fail(r, "threw: %.200s", g_ev);
        }
    } else if (SDL_strcmp(op, "reload") == 0) {
        if (eval_js(p, "location.href")) {
            SDL_strlcpy(p->url, g_ev, sizeof p->url);
        }
        const uint64_t td = SDL_GetTicksNS();
        emb_view_destroy(p->v);
        p->v = nullptr;
        SDL_Log("SCEN reload: destroying the view took %.0f ms", (double)(SDL_GetTicksNS() - td) / 1e6);
        if (!view_load(p, p->url)) {
            fail(r, "could not load %s again", p->url);
        } else {
            (void)pump(p, 50);
        }
    } else if (SDL_strcmp(op, "expect-text") == 0) {
        SDL_snprintf(js, sizeof js,
                     "(function(){var e=%s;return e?e.textContent.replace(/\\s+/g,' ').trim():'<no element>'})()", e);
        expect_js(p, js, s->text, SDL_strcmp(s->mode, "contains") == 0, ms, r);
    } else if (SDL_strcmp(op, "expect-count") == 0) {
        char want[16];
        SDL_snprintf(want, sizeof want, "%d", s->n);
        SDL_snprintf(js, sizeof js, "String(document.querySelectorAll(");
        quote_js(js, sizeof js, s->sel);
        SDL_strlcat(js, ").length)", sizeof js);
        expect_js(p, js, want, false, ms, r);
    } else if (SDL_strcmp(op, "expect-class") == 0) {
        SDL_snprintf(js, sizeof js, "(function(){var e=%s;return e?String(e.classList.contains(", e);
        quote_js(js, sizeof js, s->text);
        SDL_strlcat(js, ")):'<no element>'})()", sizeof js);
        expect_js(p, js, s->flag ? "true" : "false", false, ms, r);
    } else if (SDL_strcmp(op, "expect-value") == 0) {
        SDL_snprintf(js, sizeof js, "(function(){var e=%s;return e?String(e.value):'<no element>'})()", e);
        expect_js(p, js, s->text, false, ms, r);
    } else if (SDL_strcmp(op, "expect-checked") == 0) {
        SDL_snprintf(js, sizeof js, "(function(){var e=%s;return e?String(e.checked):'<no element>'})()", e);
        expect_js(p, js, s->flag ? "true" : "false", false, ms, r);
    } else if (SDL_strcmp(op, "expect-hash") == 0) {
        expect_js(p, "location.hash", s->text, false, ms, r);
    } else if (SDL_strcmp(op, "expect-path") == 0) {
        expect_js(p, "location.pathname", s->text, SDL_strcmp(s->mode, "contains") == 0, ms, r);
    } else if (SDL_strcmp(op, "expect-storage") == 0) {
        SDL_snprintf(js, sizeof js, "(function(){var v=localStorage.getItem(");
        quote_js(js, sizeof js, s->name);
        SDL_strlcat(js, ");return v===null?'<no item>':v})()", sizeof js);
        expect_js(p, js, s->text, true, ms, r);
    } else if (SDL_strcmp(op, "expect-focus") == 0) {
        SDL_snprintf(js, sizeof js, "String(document.activeElement===%s)", e);
        expect_js(p, js, "true", false, ms, r);
    } else if (SDL_strcmp(op, "expect-eval") == 0) {
        expect_js(p, s->js, s->text, SDL_strcmp(s->mode, "contains") == 0, ms, r);
    } else if (SDL_strcmp(op, "expect-gold") == 0) {
        expect_gold(p, s, r);
    } else if (SDL_strcmp(op, "expect-paint") == 0) {
        (void)pump(p, 30);
        emb_view_stats st;
        SDL_memset(&st, 0, sizeof st);
        st.size = sizeof st;
        emb_view_stats_get(p->v, &st);
        const uint32_t need = p->blank + (uint32_t)(s->n > 0 ? s->n : 20);
        if (st.items < need) {
            fail(r, "the frame has %u display list items, wanted at least %u (blank page %u)", st.items, need,
                 p->blank);
        }
    } else {
        fail(r, "unknown op '%s'", op);
    }
}

static void rm_tree(const char *dir);

static SDL_EnumerationResult rm_cb(void *user, const char *dirname, const char *fname)
{
    (void)user;
    char path[1400];
    SDL_snprintf(path, sizeof path, "%s%s", dirname, fname);
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        rm_tree(path);
    } else {
        SDL_RemovePath(path);
    }
    return SDL_ENUM_CONTINUE;
}

static void rm_tree(const char *dir)
{
    (void)SDL_EnumerateDirectory(dir, rm_cb, nullptr);
    SDL_RemovePath(dir);
}

/* Runs one scenario. Returns the number of passed steps; *total gets the step count, *blocked the steps not run. */
static int play_scenario(SDL_GPUDevice *dev, const char *app, uint32_t blank, int *total, int *blocked, int *errors)
{
    scenario *sc = SDL_malloc(sizeof *sc);
    const char *why = "";
    *total = *blocked = *errors = 0;
    if (sc == nullptr || !scenario_load(app, sc, &why)) {
        SDL_Log("SCEN %-16s no scenario: %s", app, why);
        SDL_free(sc);
        return 0;
    }
    *total = sc->count;

    char dist[1100];
    SDL_snprintf(dist, sizeof dist, "%s/tests/apps/%s/dist", EMBER_SOURCE_DIR, app);
    char storage[1200];
    char *cwd = SDL_GetCurrentDirectory();
    SDL_snprintf(storage, sizeof storage, "%sapps-storage-%s", cwd != nullptr ? cwd : "", app);
    SDL_free(cwd);
    rm_tree(storage);

    static play pl; /* the log callback reaches it through g_play */
    SDL_memset(&pl, 0, sizeof pl);
    pl.dev = dev;
    pl.blank = blank;
    pl.vw = sc->vw;
    pl.vh = sc->vh;
    /* a passing step settles at once; the limit only matters for failing ones, so the report run keeps it short and the
     * strict run (which must not fail on a slow machine) waits longer */
    pl.settle_ms = apps_strict() ? 300 : 100;
    const char *env = SDL_getenv("EMBER_APPS_SETTLE_MS");
    if (env != nullptr && SDL_atoi(env) > 0) {
        pl.settle_ms = SDL_atoi(env);
    }
#if EMB_TEST_ASAN
    pl.settle_ms *= 3;
#endif
    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = play_on_log;
    rd.flags = EMB_RUNTIME_NO_SYSTEM_FONTS; /* the goldens must not depend on the machine's fonts */
    pl.rt = emb_runtime_create(&rd);
    if (pl.rt == nullptr || !emb_runtime_set_storage_dir(pl.rt, storage) ||
        !emb_runtime_mount_dir(pl.rt, "app", "local", dist)) {
        SDL_Log("SCEN %-16s cannot set up the runtime (dist %s)", app, dist);
        if (pl.rt != nullptr) {
            emb_runtime_destroy(pl.rt);
        }
        SDL_free(sc);
        return 0;
    }
    g_play = &pl;
    SDL_strlcpy(pl.url, APP_URL_PREFIX "index.html", sizeof pl.url);
    pl.step_index = -1;
    const long rss_before = rss_kb();
    const size_t heap_before = embi_mem_live();
    const uint64_t t_load = SDL_GetTicksNS();
    const bool loaded = view_load(&pl, pl.url);
    SDL_Log("SCEN %-16s load: create the view and load the document took %.0f ms", app,
            (double)(SDL_GetTicksNS() - t_load) / 1e6);
    if (!loaded) {
        SDL_Log("SCEN %-16s the document did not load", app);
    } else {
        emb_view_stats st;
        SDL_memset(&st, 0, sizeof st);
        st.size = sizeof st;
        emb_view_stats_get(pl.v, &st);
        SDL_Log("SCEN %-16s load stages (ms): view creation (gpu pipelines) %.1f, fetch %.1f, parse %.1f, stylesheets %.1f, "
                "style %.1f, layout %.1f (fonts %.1f); the rest of the load is scripts",
                app, st.gpu_pipelines_ms, st.fetch_ms, st.parse_ms, st.stylesheets_ms, st.style_ms, st.layout_ms,
                st.fonts_ms);
    }
    if (loaded) {
        (void)pump(&pl, 150); /* idle: the app has booted and nothing else runs */
        SDL_Log("SCEN %-16s idle after boot: RSS %ld KB (%ld KB before the view; per view %ld KB, informational: the process "
                "also holds the GPU stack), embi_alloc heap %.2f MB (%.2f MB before)",
                app, rss_kb(), rss_before, rss_kb() - rss_before, (double)embi_mem_live() / 1048576.0,
                (double)heap_before / 1048576.0);
    }
    bool dead = !loaded;
    int passed = 0;
    for (int i = 0; i < sc->count; i++) {
        const sc_step *s = &sc->steps[i];
        pl.step_index = i;
        char desc[240];
        SDL_snprintf(desc, sizeof desc, "%s%s%s", s->op, s->sel[0] ? " " : "", s->sel);
        if (s->nth > 0) {
            char nth[24];
            SDL_snprintf(nth, sizeof nth, "[%d]", s->nth);
            SDL_strlcat(desc, nth, sizeof desc);
        }
        if (s->text[0] != '\0') {
            SDL_strlcat(desc, " \"", sizeof desc);
            SDL_strlcat(desc, s->text, sizeof desc);
            SDL_strlcat(desc, "\"", sizeof desc);
        }
        if (dead) {
            (*blocked)++;
            SDL_Log("SCEN %-16s #%02d BLOCKED %s", app, i, desc);
            continue;
        }
        step_result r;
        const uint64_t t0 = SDL_GetTicksNS();
        run_step(&pl, s, &r);
        const double step_ms = (double)(SDL_GetTicksNS() - t0) / 1e6;
        if (r.ok) {
            passed++;
            SDL_Log("SCEN %-16s #%02d PASS    %s (%.0f ms)", app, i, desc, step_ms);
        } else {
            SDL_Log("SCEN %-16s #%02d FAIL    %s: %s", app, i, desc, r.detail);
            if (s->boot) {
                dead = true;
            }
        }
        if (pl.v == nullptr) {
            dead = true; /* a failed reload */
        }
    }
    *errors = pl.errors;
    SDL_Log("SCEN %-16s RESULT passed=%d/%d blocked=%d errors=%d warnings=%d", app, passed, sc->count, *blocked,
            pl.errors, pl.warnings);
    for (int i = 0; i < pl.errors && i < ERR_KEEP; i++) {
        SDL_Log("SCEN %-16s error %d (during step %d):", app, i + 1, pl.err_step[i]);
        print_message(app, pl.err_text[i]);
    }
    const uint64_t td = SDL_GetTicksNS();
    if (pl.v != nullptr) {
        emb_view_destroy(pl.v);
    }
    const uint64_t td2 = SDL_GetTicksNS();
    g_play = nullptr;
    emb_runtime_destroy(pl.rt);
    SDL_Log("SCEN %-16s teardown: view %.0f ms, runtime %.0f ms", app, (double)(td2 - td) / 1e6,
            (double)(SDL_GetTicksNS() - td2) / 1e6);
    rm_tree(storage);
    SDL_free(sc);
    return passed;
}

EMB_TEST(embed_apps_scenarios)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        EMB_SKIP("no video subsystem");
    }
    embi_gpu *g = embi_gpu_create(nullptr, false);
    if (g == nullptr) {
        SDL_Quit();
        EMB_SKIP("no GPU device");
    }
    SDL_GPUDevice *dev = embi_gpu_device(g);
    const bool strict = apps_strict();
    const char *only = SDL_getenv("EMBER_APPS_ONLY");

    emb_runtime_desc rd;
    SDL_memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    emb_runtime *brt = emb_runtime_create(&rd);
    EMB_REQUIRE(brt != nullptr);
    const uint64_t t_blank = SDL_GetTicksNS();
    const uint32_t blank = blank_items(dev, brt);
    SDL_Log("SCEN a blank page (view, load, one frame) takes %.0f ms", (double)(SDL_GetTicksNS() - t_blank) / 1e6);
    emb_runtime_destroy(brt);

    SDL_Log("SCEN scenario player (tests/apps/<name>/scenario.json), strict=%s", strict ? "yes" : "no");
    const uint64_t t_start = SDL_GetTicksNS();
    int sum_pass = 0, sum_total = 0, sum_errors = 0;
    for (int a = 0; a < APP_COUNT; a++) {
        if (only != nullptr && SDL_strcmp(only, k_apps[a]) != 0) {
            continue;
        }
        int total = 0, blocked = 0, errors = 0;
        const int passed = play_scenario(dev, k_apps[a], blank, &total, &blocked, &errors);
        sum_pass += passed;
        sum_total += total;
        sum_errors += errors;
        if (strict) {
            EMB_CHECK(total > 0);
            EMB_CHECK(passed == total);
            EMB_CHECK(errors == 0);
        }
    }
    SDL_Log("SCEN TOTAL passed=%d/%d library errors=%d, took %.0f ms", sum_pass, sum_total, sum_errors,
            (double)(SDL_GetTicksNS() - t_start) / 1e6);
    embi_gpu_destroy(g);
    SDL_Quit();
}
