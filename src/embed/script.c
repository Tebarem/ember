/*
 * The scripts of a document (W5c): one JavaScript runtime and realm per loaded document,
 * created on the first script so a static page pays nothing. Classic scripts run from the parser's on_script hook, inline
 * and external (fetched through the view's loader, parser-blocking); defer and async apply to external scripts. The
 * DOMContentLoaded and load events fire after the parse. Script errors are logged at EMB_LOG_ERROR with the prefixes of
 * the W8a contract. The public API (ember_script.h) is at the end of the file.
 */
#include "embed/script_int.h"
#include "embed/frame.h"
#include "embed/frames.h"
#include "embed_int.h"

#include "base/clock.h"
#include "base/vec.h"
#include "bindings/bind.h"
#include "dom/dom.h"
#include "embed/input_int.h"
#include "embed/resource_int.h"
#include "embed/netload.h"
#include "embed/url.h"
#ifdef EMBER_NET
#include "net/net.h"
#endif
#include "js/js.h"
#include "loader/modules.h"

#include <stdarg.h>
#include <string.h>

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

/* A script queued for later (defer or async): everything its run needs is copied here when it is queued, so the element
 * itself can be removed from the tree (and freed) in between. The pin keeps the element's wrapper alive for
 * document.currentScript. A module script (W7d) has no pin and runs as a module, not as a classic script. */
typedef struct queued_script {
    embi_sb url;  /* the absolute URL of the source */
    uint32_t pin; /* embi_bind_pin_node token, 0 if none */
    bool module;  /* a module script */
    bool inline_module; /* an inline module: text is its source (url is empty) */
    bool inline_classic; /* an inline classic script that had to wait (for stylesheets): text is its source */
    bool net;     /* the URL is a network resource: the script is ready when its record is complete (N1f) */
    embi_sb text;
} queued_script;

struct embi_script {
    embi_doc *doc;
    embi_loop *loop;
    embi_js_runtime *rt; /* created with the realm, nullptr until the first script */
    embi_bind_ctx *ctx;
    embi_modules *mods;  /* the module host (W7d) of the realm: created with the realm */
    struct script_pre *pre; /* FP2a: a realm being built (or built) on a thread ahead of the first script, nullptr when none */
    bool pre_unwanted;     /* the document has no script: the prewarmed realm is freed as soon as its thread is done */
    emb_console_fn console;
    void *console_user;
    void (*log)(void *user, emb_log_level level, const char *text);
    void *log_user;
    EMBI_VEC(queued_script) defers; /* deferred and module scripts, in document order */
    uint32_t defer_next;            /* the next entry of defers to run */
    bool mod_wait;                  /* a module of defers was started and its imports are still downloading */
    bool mod_done;                  /* the evaluation promise of the module started last has settled */
    bool had_defers;
    EMBI_VEC(queued_script) asyncs; /* queued async scripts: each runs when it is ready (a network one when its bytes are in) */
    /* N1f: the parser-blocking script that could not run yet (its bytes or the stylesheets before it are still downloading):
     * the parser is paused on it, and the load runs it (embi_script_run_blocked) when the network has delivered. */
    struct {
        bool set;
        queued_script q;
    } blk;
    bool load_gated;  /* the load event waits for the downloads still going on */
    uint32_t active;  /* depth of the parser's script hook and of the blocked script the load runs */
    uint32_t mod_seq;     /* numbers the module evaluations watch_module attaches to */
    uint32_t mod_await;   /* the evaluation whose promise the next deferred script waits for (0: none) */
    bool mod_capture;     /* the next watch_module is the one run_defers awaits */
    uint32_t epoch;   /* bumped by every reset: a caller that ran script sees whether the document was replaced */
    int ready;        /* document.readyState: SCRIPT_LOADING, SCRIPT_INTERACTIVE or SCRIPT_COMPLETE */
    double origin_ms; /* the document's time origin (performance.now() is 0 here) */
    /* FP2a: where the time of the scripts of a load went (embi_script_stage_times). */
    double t_realm_ms;     /* taking or building the realm */
    double t_run_ms;       /* the outermost script runs (parse-time scripts, deferred and module scripts), realm and compile included */
    uint32_t t_depth;
    /* HX1: the host channel is installed on this realm, under host_name (the name given when it was installed). */
    bool host_on;
    char host_name[64];
};

enum { SCRIPT_LOADING = 0, SCRIPT_INTERACTIVE = 1, SCRIPT_COMPLETE = 2 };

/* ------------------------------------------------------------------ logging */

static void slog(embi_script *s, emb_log_level level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void slog(embi_script *s, emb_log_level level, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (s->log != nullptr) {
        s->log(s->log_user, level, buf);
        return;
    }
    emb_view *v = embi_doc_owner(s->doc);
    if (v != nullptr) {
        embi_rt_log(v->rt, level, "%s", buf);
    } else {
        SDL_Log("%s", buf);
    }
}

/* ------------------------------------------------------------------ the realm */

static void host_log(void *user, int level, embi_str text)
{
    embi_script *s = user;
    emb_log_level lv = EMB_LOG_INFO;
    if (level == EMBI_BIND_LOG_WARN) {
        lv = EMB_LOG_WARN;
    } else if (level == EMBI_BIND_LOG_ERROR) {
        lv = EMB_LOG_ERROR;
    } else if (level == EMBI_BIND_LOG_DEBUG) {
        lv = EMB_LOG_DEBUG;
    }
    if (s->console != nullptr) {
        s->console(s->console_user, lv, text.ptr, text.len);
    }
    if (lv == EMB_LOG_ERROR) {
        slog(s, EMB_LOG_ERROR, "console.error: %.*s", (int)text.len, text.ptr);
    }
}

static double host_now(void *user)
{
    (void)user;
    return embi_clock_ms();
}

static double host_time_origin(void *user)
{
    return ((embi_script *)user)->origin_ms;
}

static int host_ready_state(void *user)
{
    return ((embi_script *)user)->ready;
}

/* An error nothing handled: the line is already worded ("uncaught exception: ..."). */
static void host_uncaught(void *user, embi_str text)
{
    slog(user, EMB_LOG_ERROR, "%.*s", (int)text.len, text.ptr);
}

static embi_str host_url(void *user)
{
    embi_script *s = user;
    return embi_doc_url(s->doc);
}

/* Receives the exceptions the job queue reports (a job that threw): an ErrorEvent at the window, else the log. */
static void report_error(embi_js_runtime *rt, embi_js_value error, void *user)
{
    embi_script *s = user;
    (void)rt;
    if (s->ctx != nullptr) {
        embi_bind_report_error(s->ctx, error, nullptr, 0, 0);
    }
}

/* Receives the rejection tracker's reports: a promise rejected with no handler by the end of a job drain. The engine does
 * not expose a promise's reason outside src/js, so the line says what happened and not why. */
static void report_rejection(embi_js_runtime *rt, embi_js_value promise, int op, void *user)
{
    (void)rt;
    (void)promise;
    if (op == EMBI_JS_REJECTION_REJECT) {
        slog((embi_script *)user, EMB_LOG_ERROR, "unhandled rejection: a promise was rejected and nothing handled it");
    }
}

/* Whether the document came from the network (emb_view.net_doc), not from the embedder. */
static bool doc_is_remote(embi_script *s)
{
    emb_view *v = embi_doc_owner(s->doc);
    return v != nullptr && v->net_doc;
}

/* Whether the document's URL is http(s): a page with such a base follows its links over the network when the embedder
 * has no navigation hook, whoever loaded it. */
static bool doc_url_is_http(embi_script *s)
{
    embi_str u = embi_doc_url(s->doc);
    embi_str scheme;
    return embi_url_scheme(u, &scheme) && (embi_url_scheme_is(scheme, "http") || embi_url_scheme_is(scheme, "https"));
}

/* document.cookie of a remote document: the jar of the runtime's network service (N1d). */
static bool host_cookie_get(void *user, embi_sb *out)
{
#ifdef EMBER_NET
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    if (v == nullptr || v->rt->net == nullptr || !doc_is_remote(s)) {
        return false;
    }
    embi_str u = embi_doc_url(s->doc);
    char *z = SDL_strndup(u.ptr, u.len);
    embi_net_cookie_get(v->rt->net, z, out);
    SDL_free(z);
    return true;
#else
    (void)user;
    (void)out;
    return false;
#endif
}

static bool host_cookie_set(void *user, embi_str cookie)
{
#ifdef EMBER_NET
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    if (v == nullptr || v->rt->net == nullptr || !doc_is_remote(s)) {
        return false;
    }
    embi_str u = embi_doc_url(s->doc);
    char *z = SDL_strndup(u.ptr, u.len);
    char *c = SDL_strndup(cookie.ptr, cookie.len);
    embi_net_cookie_set(v->rt->net, z, c);
    SDL_free(c);
    SDL_free(z);
    return true;
#else
    (void)user;
    (void)cookie;
    return false;
#endif
}

static bool ensure_js(embi_script *s);

static bool host_image_pending(void *user, embi_str url)
{
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    if (v == nullptr) {
        return false;
    }
    embi_str abs = embi_doc_resolve(s->doc, url);
    return embi_netload_claims(v, abs) && embi_netload_image_pending(v, abs);
}

static bool has_attr(embi_element *el, const char *name);
static bool host_image_size(void *user, embi_str url, float *w, float *h);

typedef embi_node *node_ptr;
typedef EMBI_VEC(node_ptr) node_vec;

/* The first <img> of the document, not yet in fired, whose src resolves to url. */
static embi_node *next_image_for(embi_script *s, embi_str url, node_vec *fired)
{
    embi_document *dom = embi_doc_dom(s->doc);
    embi_node *root = &dom->base;
    for (embi_node *n = embi_next_in_tree(root, root); n != nullptr; n = embi_next_in_tree(root, n)) {
        if (!embi_is_element(n) || !embi_str_ieq(embi_atom_str(n->name), (embi_str){"img", 3})) {
            continue;
        }
        bool seen = false;
        for (uint32_t i = 0; i < fired->len && !seen; i++) {
            seen = fired->data[i] == n;
        }
        if (seen) {
            continue;
        }
        embi_sb src = {0};
        if (!embi_doc_el_attr(embi_as_element(n), "src", &src) || src.len == 0) {
            embi_sb_free(&src);
            continue;
        }
        embi_str abs = embi_doc_resolve(s->doc, (embi_str){src.data, src.len});
        embi_sb_free(&src);
        for (uint32_t i = 0; i < abs.len; i++) {
            if (abs.ptr[i] == '#') {
                abs.len = i;
                break;
            }
        }
        if (embi_str_eq(abs, url)) {
            return n;
        }
    }
    return nullptr;
}

/* A network image arrived (ok) or failed: load or error at each <img> of that URL. Without a realm only an img with an onload
 * or onerror attribute makes one (nothing else can be listening); a scripting-off document is not asked at all. Handlers
 * may remove nodes, so the tree is searched again after every event and a node is fired once. */
void embi_script_image_event(embi_script *s, embi_str url, bool ok)
{
    if (embi_doc_dom(s->doc) == nullptr) {
        return;
    }
    emb_view *v = embi_doc_owner(s->doc);
    if (ok && v != nullptr && embi_view_gfx_ready(v)) {
        float w = 0, h = 0;
        ok = host_image_size(s, url, &w, &h); /* the bytes arrived: they have to decode as an image, too */
    }
    node_vec fired = {0};
    for (uint32_t guard = 0; guard < 4096; guard++) {
        embi_node *n = next_image_for(s, url, &fired);
        if (n == nullptr) {
            break;
        }
        embi_vec_push(&fired, n);
        if (s->ctx == nullptr) {
            embi_element *el = embi_as_element(n);
            if (!has_attr(el, "onload") && !has_attr(el, "onerror")) {
                continue;
            }
        }
        if (!ensure_js(s)) {
            break;
        }
        embi_bind_fire_node_event(s->ctx, n, ok ? "load" : "error");
        embi_js_run_jobs(s->rt);
    }
    embi_vec_free(&fired);
}

/* An http(s) document does not navigate to file:, app: or another scheme of the embedder (one warning); the schemes of the
 * web and the links that hand off to the system (mailto:, tel:) are left alone. */
static bool nav_scheme_ok(embi_script *s, embi_str url)
{
    embi_str us;
    if (!doc_is_remote(s) || !embi_url_scheme(url, &us)) {
        return true;
    }
    static const char *const ok[] = {"http", "https", "data", "blob", "about", "javascript", "mailto", "tel"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        if (embi_url_scheme_is(us, ok[i])) {
            return true;
        }
    }
    slog(s, EMB_LOG_WARN, "net: a page from the network cannot navigate to %.*s (local resources are refused)",
         (int)(url.len > 200 ? 200 : url.len), url.ptr);
    return false;
}

/* The embedder's navigate hook (location assignments, link activations of the bindings): the view's navigation callback.
 * Fragment navigations stay in the bindings and do not come here. */
static bool host_navigate(void *user, embi_str url, bool replace)
{
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    (void)replace;
    if (v == nullptr) {
        return false;
    }
    if (!nav_scheme_ok(s, url)) {
        return false;
    }
    if (embi_frames_navigate(v, url)) {
        return true; /* IF1: a frame navigates itself (at its parent's next update) */
    }
    if (!embi_input_has_navigate(v) && (doc_is_remote(s) || doc_url_is_http(s)) && embi_netload_allowed(v, url)) {
        /* A remote page with no navigation hook of the embedder: the view follows the link itself (N1d). */
        return embi_netload_navigate(v, url);
    }
    embi_input_navigate(v, url);
    return true;
}

/* A form submission: the form-aware navigation hook of the view, when it has one. */
static bool host_navigate_form(void *user, embi_str method, embi_str url, embi_str body, embi_str ctype)
{
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    return v != nullptr && nav_scheme_ok(s, url) && embi_input_navigate_form(v, method, url, body, ctype);
}

static bool host_image_size(void *user, embi_str url, float *w, float *h)
{
    embi_script *s = user;
    return embi_doc_image_natural(s->doc, url, w, h);
}

/* window.scrollTo and scrollIntoView: the view's root moves to the offsets. */
static void host_scroll(void *user, double x, double y)
{
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    if (v != nullptr) {
        emb_view_scroll_to(v, (float)x, (float)y);
    }
}

/* window.scrollX/scrollY and the client rectangles: the viewport's offset now (a wheel or a smooth scroll moves it). */
static void host_root_offset(void *user, double *x, double *y)
{
    embi_script *s = user;
    float fx = 0.0f, fy = 0.0f;
    embi_doc_root_offset(s->doc, &fx, &fy);
    *x = (double)fx;
    *y = (double)fy;
}

/* scrollTop and scrollLeft of a scroll container element: the offsets live on its fragment. */
static bool host_scroll_el(void *user, embi_element *el, bool set, double *x, double *y)
{
    embi_script *s = user;
    float cx = 0.0f, cy = 0.0f;
    if (!embi_doc_scroller_offset(s->doc, el, &cx, &cy)) {
        return false;
    }
    if (set) {
        (void)embi_doc_scroll_key_by(s->doc, el, (float)*x - cx, (float)*y - cy, 0.0f, 0.0f);
        if (!embi_doc_scroller_offset(s->doc, el, &cx, &cy)) {
            return false;
        }
    }
    *x = (double)cx;
    *y = (double)cy;
    return true;
}

/* window.innerWidth/innerHeight and devicePixelRatio (W8b): the document's viewport, which the view sets from its size and
 * scale. A document without a viewport yet keeps the realm's defaults (1024 x 768 at 1x). */
static void host_viewport(void *user, double *w, double *h, double *dpr)
{
    embi_script *s = user;
    float vw = 0, vh = 0, dppx = 0;
    embi_doc_viewport(s->doc, &vw, &vh, &dppx);
    if (vw > 0 && vh > 0) {
        *w = vw;
        *h = vh;
    }
    if (dppx > 0) {
        *dpr = dppx;
    }
}

/* The module host's loads: through the view's runtime (the handlers, with the media type), or through the document's loader
 * for a document made without a view. Synchronous, like every document load. */
static bool mod_load(void *user, embi_str url, uint8_t **data, size_t *len, embi_sb *mime)
{
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    if (v != nullptr && v->rt != nullptr) {
        if (!embi_netload_scheme_ok(v, url)) {
            return false;
        }
        if (embi_netload_claims(v, url)) {
            return embi_netload_get(v, url, 0, data, len, mime) == EMBI_NL_OK;
        }
        return embi_rt_load(v->rt, url, data, len, mime);
    }
    return embi_doc_fetch_abs(s->doc, url, data, len);
}

/* A module that failed to load only because it is still downloading (the engine's request waits for it). */
static bool mod_is_pending(void *user, embi_str url)
{
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    return v != nullptr && embi_netload_claims(v, url) && embi_netload_scheme_ok(v, url) &&
           embi_netload_state(v, url) == EMBI_NL_PENDING;
}

static void mod_prefetch(void *user, embi_str url)
{
    embi_script *s = user;
    emb_view *v = embi_doc_owner(s->doc);
    if (v != nullptr && embi_netload_claims(v, url) && embi_netload_scheme_ok(v, url)) {
        embi_netload_prefetch(v, url);
    }
}

static void mod_warn(void *user, const char *text)
{
    slog(user, EMB_LOG_WARN, "%s", text);
}

/* document.adoptedStyleSheets changed: the document's cascade takes the sheets (W7d). */
static void host_adopt(void *user, void *const *sheets, uint32_t n)
{
    embi_script *s = user;
    embi_doc_set_adopted(s->doc, (embi_css_sheet *const *)sheets, n);
}

/* The bindings are about to read styles or geometry: the sheets are brought up to date (W6e). */
static void host_sync_sheets(void *user)
{
    embi_script *s = user;
    embi_doc_sync_sheets(s->doc);
}

/* ------------------------------------------------------------------ the realm built ahead (FP2a)
 *
 * Building the realm (a runtime and the 400 or so interfaces of the bindings: 1.7 to 2.5 ms, most of it allocating and
 * interning) used to happen on the first script, on the critical path of the first paint. A thread now builds it while the
 * view is created and the document engines are made. The realm needs nothing of the document (embi_bind_new_detached), so
 * the thread and the main thread share nothing; the first script joins the thread, takes the runtime over and binds the
 * document. A document that turns out to have no script lets the realm go as soon as the thread is done (never waiting for
 * it); destroying the script state joins it. */
enum { PRE_BUILDING = 0, PRE_READY = 1, PRE_UNWANTED = 2 };

typedef struct script_pre {
    SDL_Thread *th;
    SDL_AtomicInt done; /* 1 once rt and ctx are set (or the build failed), or once the thread has freed them */
    SDL_AtomicInt state; /* PRE_BUILDING, PRE_READY (the realm waits for its owner) or PRE_UNWANTED (the thread frees it) */
    bool freeing;        /* main thread only: a thread is freeing the unused realm */
    embi_js_runtime *rt;
    embi_bind_ctx *ctx;
} script_pre;

/* Frees the realm of p (on whichever thread calls it: the main thread never pays for a realm nobody used). */
static void pre_free_realm(script_pre *p)
{
    if (p->ctx != nullptr) {
        embi_js_adopt_thread(p->rt);
        embi_bind_free(p->ctx);
        embi_js_runtime_free(p->rt);
    }
    p->ctx = nullptr;
    p->rt = nullptr;
}

static int pre_free_main(void *arg)
{
    script_pre *p = arg;
    pre_free_realm(p);
    SDL_SetAtomicInt(&p->done, 1);
    return 0;
}

static int pre_main(void *arg)
{
    script_pre *p = arg;
    p->rt = embi_js_runtime_new();
    if (p->rt != nullptr) {
        p->ctx = embi_bind_new_detached(p->rt);
        if (p->ctx == nullptr) {
            embi_js_runtime_free(p->rt);
            p->rt = nullptr;
        }
    }
    if (p->rt != nullptr) {
        /* The realm's own garbage goes now, on this thread, so that the first script starts with a fresh allocation budget. */
        embi_js_gc(p->rt);
    }
    if (!SDL_CompareAndSwapAtomicInt(&p->state, PRE_BUILDING, PRE_READY)) {
        pre_free_realm(p); /* the document turned out not to need it while it was being built */
    }
    SDL_SetAtomicInt(&p->done, 1);
    return 0;
}

/* Waits for the thread of p (it ends right after it sets done). */
static void pre_join(script_pre *p)
{
    if (p->th != nullptr) {
        SDL_WaitThread(p->th, nullptr);
        p->th = nullptr;
    }
}

static void pre_discard(embi_script *s)
{
    script_pre *p = s->pre;
    if (p == nullptr) {
        return;
    }
    s->pre = nullptr;
    (void)SDL_CompareAndSwapAtomicInt(&p->state, PRE_BUILDING, PRE_UNWANTED);
    pre_join(p);
    pre_free_realm(p);
    embi_free(p, sizeof *p);
}

static void pre_start(embi_script *s)
{
    if (s->pre != nullptr || s->ctx != nullptr) {
        return;
    }
    script_pre *p = embi_calloc(1, sizeof *p);
    p->th = SDL_CreateThread(pre_main, "ember-realm", p);
    if (p->th == nullptr) {
        embi_free(p, sizeof *p); /* no thread: the first script builds the realm itself */
        return;
    }
    s->pre = p;
}

/* Frees the prewarmed realm once its thread has finished, when the document will not use it. */
static void pre_reap(embi_script *s)
{
    script_pre *p = s->pre;
    if (p == nullptr || !s->pre_unwanted || SDL_GetAtomicInt(&p->done) == 0) {
        return;
    }
    pre_join(p); /* the thread has finished: this only collects it */
    if (p->ctx != nullptr && !p->freeing) {
        /* A finished realm nobody wants: another thread frees it, so that the main thread does not. */
        SDL_SetAtomicInt(&p->done, 0);
        p->freeing = true;
        p->th = SDL_CreateThread(pre_free_main, "ember-realm-free", p);
        if (p->th != nullptr) {
            return;
        }
        SDL_SetAtomicInt(&p->done, 1);
    }
    s->pre = nullptr;
    pre_free_realm(p);
    embi_free(p, sizeof *p);
}

bool embi_script_prewarm_held(const embi_script *s)
{
    return s->pre != nullptr;
}

void embi_script_no_prewarm(embi_script *s)
{
    s->pre_unwanted = true;
    if (s->pre != nullptr) {
        (void)SDL_CompareAndSwapAtomicInt(&s->pre->state, PRE_BUILDING, PRE_UNWANTED);
    }
    pre_reap(s);
}

/* A document with this markup and no script in it does not need the realm built ahead of time. */
void embi_script_hint_markup(embi_script *s, embi_str html)
{
    if (s->pre == nullptr || s->ctx != nullptr) {
        return;
    }
    for (size_t i = 0; i + 7 <= html.len; i++) {
        if (html.ptr[i] == '<' && (html.ptr[i + 1] | 0x20) == 's' && (html.ptr[i + 2] | 0x20) == 'c' &&
            (html.ptr[i + 3] | 0x20) == 'r' && (html.ptr[i + 4] | 0x20) == 'i' && (html.ptr[i + 5] | 0x20) == 'p' &&
            (html.ptr[i + 6] | 0x20) == 't') {
            return;
        }
    }
    embi_script_no_prewarm(s);
}

/* Takes the prewarmed runtime and context, if there are any. */
static bool pre_take(embi_script *s, embi_js_runtime **rt, embi_bind_ctx **ctx)
{
    script_pre *p = s->pre;
    if (p == nullptr) {
        return false;
    }
    s->pre = nullptr;
    pre_join(p);
    *rt = p->rt;
    *ctx = p->ctx;
    embi_free(p, sizeof *p);
    if (*ctx == nullptr) {
        return false;
    }
    embi_js_adopt_thread(*rt);
    return true;
}

/* ------------------------------------------------------------------ the host channel (HX1) */

/* postMessage of the channel object: calls the host's receiver with String(message). The view is the function's data. */
static embi_js_value host_post_message(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv,
                                       int magic)
{
    (void)this_v;
    (void)magic;
    emb_view *v = embi_js_native_data_current(r);
    embi_js_runtime *rt = r->rt;
    if (argc < 1) {
        return embi_js_throw_type_error(rt, "postMessage needs a message");
    }
    if (v == nullptr || v->host_fn == nullptr) {
        return EMBI_JS_UNDEFINED;
    }
    embi_js_string *str = embi_js_to_string(rt, argv[0]);
    if (str == nullptr) {
        return EMBI_JS_EXCEPTION;
    }
    embi_sb sb = {0};
    embi_js_str_append_utf8(str, &sb);
    v->host_fn(v->host_user, (const char *)sb.data, sb.len);
    embi_sb_free(&sb);
    return EMBI_JS_UNDEFINED;
}

/* Installs the channel object on the realm of s when the view has a channel and the document is the view's own. The
 * global property is read-only and not deletable; onMessage starts null. A realm keeps the first name it got. */
static void host_channel_install(embi_script *s, emb_view *v)
{
    if (v == nullptr || !v->host_channel || s->host_on || s->ctx == nullptr || v->doc != s->doc) {
        return;
    }
    embi_js_runtime *rt = s->rt;
    embi_js_realm *r = embi_bind_realm(s->ctx);
    embi_js_object *chan = embi_js_object_new_plain(rt);
    embi_js_object *post = embi_js_native_function_new_data(r, "postMessage", 1, host_post_message, 0, v);
    embi_js_pdesc d;
    memset(&d, 0, sizeof d);
    d.value = embi_js_from_object(post);
    d.has = EMBI_JS_PD_VALUE | EMBI_JS_PD_WRITABLE | EMBI_JS_PD_ENUMERABLE | EMBI_JS_PD_CONFIGURABLE;
    d.attrs = EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE;
    if (embi_js_define_property_or_throw(rt, chan, embi_js_key_c(rt, "postMessage"), &d) != EMBI_JS_YES) {
        embi_js_exception_clear(rt);
        return;
    }
    if (embi_js_create_data_property_or_throw(rt, chan, embi_js_key_c(rt, "onMessage"), EMBI_JS_NULL) !=
        EMBI_JS_YES) {
        embi_js_exception_clear(rt);
        return;
    }
    d.value = embi_js_from_object(chan);
    d.attrs = 0; /* not writable, not enumerable, not configurable */
    if (embi_js_define_property_or_throw(rt, r->global, embi_js_key_c(rt, v->host_name), &d) != EMBI_JS_YES) {
        embi_js_exception_clear(rt);
        slog(s, EMB_LOG_WARN, "host channel: the global \"%s\" could not be defined", v->host_name);
        return;
    }
    memcpy(s->host_name, v->host_name, sizeof s->host_name);
    s->host_on = true;
}

static bool ensure_js_inner(embi_script *s);

/* Makes the realm of the document if it does not exist yet. False when there is no document or no memory. */
static bool ensure_js(embi_script *s)
{
    if (s->ctx != nullptr) {
        return true;
    }
    const uint64_t t0 = SDL_GetPerformanceCounter();
    bool ok = ensure_js_inner(s);
    s->t_realm_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    return ok;
}

static bool ensure_js_inner(embi_script *s)
{
    embi_document *dom = embi_doc_dom(s->doc);
    if (dom == nullptr) {
        return false;
    }
    embi_js_runtime *rt = nullptr;
    embi_bind_ctx *c = nullptr;
    if (pre_take(s, &rt, &c)) {
        embi_bind_attach_doc(c, dom);
    } else {
        rt = embi_js_runtime_new();
        if (rt == nullptr) {
            slog(s, EMB_LOG_ERROR, "scripts: the JavaScript runtime could not be created");
            return false;
        }
        c = embi_bind_new(rt, dom);
        if (c == nullptr) {
            embi_js_runtime_free(rt);
            slog(s, EMB_LOG_ERROR, "scripts: the realm could not be created");
            return false;
        }
    }
    s->rt = rt;
    s->ctx = c;
    embi_bind_host h;
    memset(&h, 0, sizeof h);
    h.user = s;
    h.log = host_log;
    h.now_ms = host_now;
    h.url = host_url;
    h.time_origin_ms = host_time_origin;
    h.ready_state = host_ready_state;
    h.uncaught = host_uncaught;
    h.adopt = host_adopt;
    h.navigate = host_navigate;
    h.navigate_form = host_navigate_form;
    h.scroll = host_scroll;
    h.root_offset = host_root_offset;
    h.viewport = host_viewport;
    h.image_size = host_image_size;
    h.scroll_el = host_scroll_el;
    h.sync_sheets = host_sync_sheets;
    h.cookie_get = host_cookie_get;
    h.cookie_set = host_cookie_set;
    h.image_pending = host_image_pending;
    h.style = embi_doc_style_engine(s->doc); /* geometry and computed style read the document's engines */
    h.layout = embi_doc_layout_engine(s->doc);
    h.animator = embi_doc_animator(s->doc); /* W6c: Element.animate and the animation objects */
    embi_bind_set_host(c, &h);
    embi_doc_install_svg_geometry(c, s->doc);
    embi_bind_timers t = {.user = s->loop, .add = embi_loop_timer_add, .clear = embi_loop_timer_clear};
    embi_bind_set_timers(c, &t);
    embi_rt_bind_ctx(c, embi_doc_owner(s->doc)); /* fetch, blob: and storage hooks of the view (W7c) */
    embi_js_set_host_report_error(rt, report_error, s);
    embi_js_set_host_rejection_tracker(rt, report_rejection, s);
    embi_mod_host mh = {.user = s, .load = mod_load, .warn = mod_warn, .prefetch = mod_prefetch,
                      .is_pending = mod_is_pending};
    s->mods = embi_modules_new(rt, embi_bind_realm(c), &mh);
    embi_modules_set_doc_url(s->mods, embi_doc_url(s->doc));
    embi_modules_attach(s->mods);
    host_channel_install(s, embi_doc_owner(s->doc));
    return true;
}

embi_bind_ctx *embi_script_bind(embi_script *s)
{
    return ensure_js(s) ? s->ctx : nullptr;
}

embi_bind_ctx *embi_script_bind_if(embi_script *s)
{
    return s->ctx;
}

double embi_script_time_ms(embi_script *s)
{
    return embi_clock_ms() - s->origin_ms;
}

void embi_script_stage_times(const embi_script *s, double *realm_ms, double *compile_ms, double *run_ms)
{
    double compile = s->rt != nullptr ? embi_js_compile_ms(s->rt) : 0.0;
    double realm = s->t_realm_ms;
    double run = s->t_run_ms - compile - realm; /* the runs include the realm and the compile they triggered */
    *realm_ms = realm;
    *compile_ms = compile;
    *run_ms = run > 0.0 ? run : 0.0;
}

/* ------------------------------------------------------------------ running code */

/* Writes the text of a value (its ToString, or "[unprintable]") into out. */
static void describe(embi_script *s, embi_js_value v, embi_sb *out)
{
    embi_js_string *str = embi_js_to_string(s->rt, v);
    if (str == nullptr) {
        embi_js_exception_clear(s->rt);
        embi_sb_append(out, EMBI_STR("[unprintable]"));
        return;
    }
    embi_js_str_append_utf8(str, out);
}

/* Reports the exception that is pending on the runtime: an ErrorEvent at the window, else a log line with its location. */
static void report_exception(embi_script *s)
{
    embi_js_value ex = embi_js_exception_get(s->rt);
    embi_js_exception_clear(s->rt);
    const char *file = nullptr;
    uint32_t line = 0, col = 0;
    (void)embi_js_error_location(s->rt, &file, &line, &col);
    embi_bind_report_error(s->ctx, ex, file != nullptr ? file : "?", line, col);
}

/* Runs one classic script: evaluates it, reports a thrown error, then runs the microtasks it queued. */
static void eval_source(embi_script *s, const char *src, size_t len, const char *file)
{
    if (!ensure_js(s)) {
        return;
    }
    embi_js_value v = embi_js_eval(embi_bind_realm(s->ctx), src, len, file, 0);
    if (v == EMBI_JS_EXCEPTION) {
        report_exception(s);
    }
    embi_js_run_jobs(s->rt);
}

static bool has_attr(embi_element *el, const char *name)
{
    embi_sb tmp = {0};
    bool present = embi_doc_el_attr(el, name, &tmp);
    embi_sb_free(&tmp);
    return present;
}

/* The MIME type essence of a script's type attribute (lowercase, without parameters), copied into buf. */
static void mime_essence(embi_str v, char *buf, size_t cap)
{
    size_t n = 0;
    for (uint32_t i = 0; i < v.len && v.ptr[i] != ';' && n + 1 < cap; i++) {
        char ch = v.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        buf[n++] = ch;
    }
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        n--;
    }
    size_t start = 0;
    while (start < n && (buf[start] == ' ' || buf[start] == '\t' || buf[start] == '\n' || buf[start] == '\r')) {
        start++;
    }
    if (start > 0) {
        memmove(buf, buf + start, n - start);
        n -= start;
    }
    buf[n] = '\0';
}

static bool is_js_mime(const char *t)
{
    static const char *const types[] = {
        "application/ecmascript", "application/javascript", "application/x-ecmascript",
        "application/x-javascript", "text/ecmascript", "text/javascript", "text/javascript1.0",
        "text/javascript1.1", "text/javascript1.2", "text/javascript1.3", "text/javascript1.4",
        "text/javascript1.5", "text/jscript", "text/livescript", "text/x-ecmascript", "text/x-javascript",
    };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (SDL_strcmp(t, types[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The type of a script element as HTML decides it: the essence of a non-empty type attribute; else "text/" plus a
 * non-empty language attribute; else "text/javascript". Written to essence (lowercase, no parameters). */
static void script_essence(embi_element *el, char *essence, size_t cap)
{
    embi_sb type = {0};
    embi_sb lang = {0};
    if (embi_doc_el_attr(el, "type", &type) && type.len > 0) {
        mime_essence((embi_str){type.data, type.len}, essence, cap);
    } else if (embi_doc_el_attr(el, "language", &lang) && lang.len > 0) {
        embi_sb full = {0};
        embi_sb_append(&full, EMBI_STR("text/"));
        embi_sb_append(&full, (embi_str){lang.data, lang.len});
        mime_essence((embi_str){full.data, full.len}, essence, cap);
        embi_sb_free(&full);
    } else {
        SDL_strlcpy(essence, "text/javascript", cap);
    }
    embi_sb_free(&type);
    embi_sb_free(&lang);
}

/* Whether a script element's type attribute has the token (a space-separated list, ASCII case-insensitive). */
static bool attr_has_token(embi_element *el, const char *name, const char *token)
{
    embi_sb v = {0};
    bool found = false;
    if (embi_doc_el_attr(el, name, &v)) {
        size_t tl = SDL_strlen(token);
        uint32_t i = 0;
        while (i < v.len && !found) {
            while (i < v.len && (v.data[i] == ' ' || v.data[i] == '\t' || v.data[i] == '\n' || v.data[i] == '\r')) {
                i++;
            }
            uint32_t start = i;
            while (i < v.len && !(v.data[i] == ' ' || v.data[i] == '\t' || v.data[i] == '\n' || v.data[i] == '\r')) {
                i++;
            }
            found = i - start == tl && SDL_strncasecmp(v.data + start, token, tl) == 0;
        }
    }
    embi_sb_free(&v);
    return found;
}

/* Appends the absolute URL of a script element's src to out. False when there is no src or it is empty. */
static bool script_src(embi_script *s, embi_element *el, embi_sb *out)
{
    embi_sb src = {0};
    if (!embi_doc_el_attr(el, "src", &src)) {
        embi_sb_free(&src);
        return false;
    }
    if (src.len == 0) {
        embi_sb_free(&src);
        slog(s, EMB_LOG_WARN, "script: the src attribute is empty");
        return false;
    }
    embi_str abs = embi_doc_resolve(s->doc, (embi_str){src.data, src.len});
    embi_sb_free(&src);
    embi_sb_append(out, abs);
    return true;
}

/* Fetches url and runs it as a classic script. The script's URL is the base of the import() calls it makes (W7d). */
static void run_url(embi_script *s, embi_str url)
{
    uint8_t *data = nullptr;
    size_t len = 0;
    if (!embi_doc_fetch_abs(s->doc, url, &data, &len)) {
        slog(s, EMB_LOG_WARN, "script: cannot load %.*s", (int)url.len, url.ptr);
        return;
    }
    embi_sb name = {0};
    embi_sb_append(&name, url);
    if (s->mods != nullptr) {
        embi_modules_set_script_url(s->mods, url);
    }
    eval_source(s, (const char *)data, len, embi_sb_cstr(&name));
    if (s->mods != nullptr) {
        embi_modules_set_script_url(s->mods, (embi_str){nullptr, 0});
    }
    embi_sb_free(&name);
    SDL_free(data);
}

/* ------------------------------------------------------------------ module scripts (W7d) */

/* The rejection of a module's evaluation: its error is reported as an uncaught error (an ErrorEvent at the window, else
 * the log), which the promise would otherwise hide from the rejection tracker. */
static embi_js_value module_rejected(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv,
                                     int magic)
{
    (void)this_v;
    embi_script *s = embi_js_native_data_current(r);
    embi_js_value err = argc > 0 ? argv[0] : EMBI_JS_UNDEFINED;
    if (s != nullptr && (uint32_t)magic == s->mod_await) {
        s->mod_done = true;
    }
    if (s != nullptr && s->ctx != nullptr) {
        embi_bind_report_error(s->ctx, err, nullptr, 0, 0);
    }
    return EMBI_JS_UNDEFINED;
}

/* The evaluation of a module finished (N1f: the next deferred script waits for it, not for imports it makes later). */
static embi_js_value module_fulfilled(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv, int magic)
{
    (void)this_v;
    (void)argc;
    (void)argv;
    embi_script *s = embi_js_native_data_current(r);
    if (s != nullptr && (uint32_t)magic == s->mod_await) {
        s->mod_done = true;
    }
    return EMBI_JS_UNDEFINED;
}

/* Attaches the rejection report to the promise of a module evaluation. */
static void watch_module(embi_script *s, embi_js_value promise)
{
    if (!embi_js_is_object(promise) || s->ctx == nullptr) {
        return;
    }
    /* The handlers know which evaluation they belong to: only the one the deferred queue awaits releases it. */
    int tok = (int)++s->mod_seq;
    if (s->mod_capture) {
        s->mod_capture = false;
        s->mod_await = (uint32_t)tok;
        s->mod_done = false;
    }
    embi_js_object *fn = embi_js_native_function_new_data(embi_bind_realm(s->ctx), "moduleRejected", 1,
                                                          module_rejected, tok, s);
    embi_js_object *ok = embi_js_native_function_new_data(embi_bind_realm(s->ctx), "moduleDone", 1, module_fulfilled, tok, s);
    if (fn != nullptr) {
        (void)embi_js_promise_then(s->rt, embi_js_as_object(promise), ok != nullptr ? embi_js_from_object(ok) : EMBI_JS_UNDEFINED,
                                   embi_js_from_object(fn), nullptr);
    }
}

/* Runs the module at an absolute URL (an external module script) and the microtasks it queued. */
static void run_module_url(embi_script *s, embi_str url)
{
    if (!ensure_js(s)) {
        return;
    }
    watch_module(s, embi_modules_eval_url(s->mods, url));
    embi_js_run_jobs(s->rt);
}

/* Runs an inline module script: its text is the module, and its URL is the document's. */
static void run_inline_module(embi_script *s, embi_str text)
{
    if (!ensure_js(s)) {
        return;
    }
    watch_module(s, embi_modules_eval_source(s->mods, text));
    embi_js_run_jobs(s->rt);
}

/* Parses an import map element (type importmap) and merges it into the document's maps. A bad map is reported and
 * ignored as a whole. */
static void run_importmap(embi_script *s, embi_element *el)
{
    if (!ensure_js(s)) {
        return;
    }
    embi_sb text = {0};
    embi_sb err = {0};
    embi_doc_el_text(el, &text);
    if (!embi_modules_add_importmap(s->mods, (embi_str){text.data, text.len}, &err)) {
        slog(s, EMB_LOG_ERROR, "importmap: %s", embi_sb_cstr(&err));
    }
    embi_sb_free(&err);
    embi_sb_free(&text);
}

/* Fetches the module preloads of the document (<link rel=modulepreload>) before the scripts run, so the modules they
 * name are in the module cache when the scripts import them. */
static void preload_links(embi_script *s)
{
    embi_document *dom = embi_doc_dom(s->doc);
    if (dom == nullptr) {
        return;
    }
    embi_node *root = &dom->base;
    for (embi_node *n = embi_next_in_tree(root, root); n != nullptr; n = embi_next_in_tree(root, n)) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *el = embi_as_element(n);
        if (!attr_has_token(el, "rel", "modulepreload")) {
            continue;
        }
        embi_sb href = {0};
        if (embi_doc_el_attr(el, "href", &href) && href.len > 0 && ensure_js(s)) {
            embi_str abs = embi_doc_resolve(s->doc, (embi_str){href.data, href.len});
            if (abs.len > 0) {
                (void)embi_modules_preload(s->mods, abs);
            }
        }
        embi_sb_free(&href);
    }
}

/* Runs a script element now (the parser is blocked on it): its src or its text, with document.currentScript set. */
static void run_element(embi_script *s, embi_element *el)
{
    if (!ensure_js(s)) {
        return;
    }
    embi_sb url = {0};
    bool external = has_attr(el, "src");
    if (external && !script_src(s, el, &url)) {
        embi_sb_free(&url);
        return;
    }
    embi_bind_set_current_script(s->ctx, &el->base);
    if (external) {
        run_url(s, (embi_str){url.data, url.len});
    } else {
        embi_sb text = {0};
        embi_doc_el_text(el, &text);
        eval_source(s, text.len > 0 ? text.data : "", text.len, "inline script");
        embi_sb_free(&text);
    }
    if (s->ctx != nullptr) { /* the script may have replaced the document */
        embi_bind_set_current_script(s->ctx, nullptr);
    }
    embi_sb_free(&url);
}

/* Runs a queued script and releases its copies. */
static void run_queued_inner(embi_script *s, queued_script *q);

static void run_queued(embi_script *s, queued_script *q)
{
    const uint64_t t0 = SDL_GetPerformanceCounter();
    s->t_depth++;
    run_queued_inner(s, q);
    if (--s->t_depth == 0) {
        s->t_run_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    }
}

static void run_queued_inner(embi_script *s, queued_script *q)
{
    if (q->inline_module) {
        run_inline_module(s, (embi_str){q->text.data, q->text.len});
    } else if (q->inline_classic) {
        if (ensure_js(s)) {
            embi_bind_set_current_script_token(s->ctx, q->pin);
            eval_source(s, q->text.len > 0 ? q->text.data : "", q->text.len, "inline script");
            if (s->ctx != nullptr) {
                embi_bind_set_current_script_token(s->ctx, 0);
                embi_bind_unpin_node(s->ctx, q->pin);
            }
        }
    } else if (q->module) {
        run_module_url(s, (embi_str){q->url.data, q->url.len});
    } else if (ensure_js(s)) {
        embi_bind_set_current_script_token(s->ctx, q->pin);
        run_url(s, (embi_str){q->url.data, q->url.len});
        if (s->ctx != nullptr) {
            embi_bind_set_current_script_token(s->ctx, 0);
            embi_bind_unpin_node(s->ctx, q->pin);
        }
    }
    embi_sb_free(&q->url);
    embi_sb_free(&q->text);
    q->pin = 0;
}

/* ------------------------------------------------------------------ the parser hook */

static void task_async(void *user);

/* Copies what a deferred or async script needs (its URL, and for a classic script a pin on its element) into a queue
 * entry. */
static bool queue_script(embi_script *s, embi_element *el, bool module, queued_script *q)
{
    memset(q, 0, sizeof *q);
    q->module = module;
    if (!script_src(s, el, &q->url)) {
        embi_sb_free(&q->url);
        return false;
    }
    q->pin = !module && ensure_js(s) ? embi_bind_pin_node(s->ctx, &el->base) : 0;
    /* A network script that will run later starts downloading now (several scripts download in parallel). */
    emb_view *v = embi_doc_owner(s->doc);
    if (v != nullptr && embi_netload_claims(v, (embi_str){q->url.data, q->url.len}) &&
        embi_netload_scheme_ok(v, (embi_str){q->url.data, q->url.len})) {
        q->net = true;
        embi_netload_prefetch(v, (embi_str){q->url.data, q->url.len});
    }
    return true;
}

/* Whether the source of a queued script is still downloading. */
static bool queued_pending(embi_script *s, const queued_script *q)
{
    emb_view *v = embi_doc_owner(s->doc);
    return q->net && v != nullptr && embi_netload_state(v, (embi_str){q->url.data, q->url.len}) == EMBI_NL_PENDING;
}

/* Whether scripts wait for stylesheets: a <link rel=stylesheet> of the document so far is still downloading (a script may read
 * the styles it would give). Only a document that has network loads asks; the others never walk the tree. */
static bool sheets_block_scripts(embi_script *s)
{
    return embi_doc_sheets_pending(s->doc); /* the sheets the parser inserted (noted as they were); none without a network */
}

/* A classic script about to run in the parser that has to wait: its source is still downloading, or a stylesheet before it is.
 * Fills q with what the run needs (copied, the element may go meanwhile) and returns true; the parser is then paused. */
static bool script_must_wait(embi_script *s, embi_element *el, bool external, queued_script *q)
{
    emb_view *v = embi_doc_owner(s->doc);
    if (v == nullptr || v->rt == nullptr) {
        return false;
    }
    bool wait = false;
    if (external) {
        embi_sb src = {0};
        bool has = embi_doc_el_attr(el, "src", &src) && src.len > 0;
        embi_sb_free(&src);
        if (!has) {
            return false; /* run_element words the empty src */
        }
        embi_sb url = {0};
        if (script_src(s, el, &url)) {
            wait = embi_netload_claims(v, (embi_str){url.data, url.len}) &&
                   embi_netload_scheme_ok(v, (embi_str){url.data, url.len}) &&
                   embi_netload_state(v, (embi_str){url.data, url.len}) == EMBI_NL_PENDING;
        }
        embi_sb_free(&url);
    }
    wait = wait || sheets_block_scripts(s);
    if (!wait) {
        return false;
    }
    if (external) {
        return queue_script(s, el, false, q);
    }
    memset(q, 0, sizeof *q);
    q->inline_classic = true;
    embi_doc_el_text(el, &q->text);
    q->pin = ensure_js(s) ? embi_bind_pin_node(s->ctx, &el->base) : 0;
    return true;
}

/* The parser's hook. Import maps are read here, module scripts run or are queued (deferred by default when external,
 * async when async), and classic scripts are as before. A classic script with nomodule is skipped (a module-capable
 * browser runs modules instead). */
static embi_html_script_result on_script_inner(void *ctx, embi_element *el);

embi_html_script_result embi_script_on_script(void *ctx, embi_element *el)
{
    embi_script *s = ctx;
    const uint32_t epoch = s->epoch;
    const uint64_t t0 = SDL_GetPerformanceCounter();
    s->t_depth++;
    s->active++;
    embi_html_script_result r = on_script_inner(ctx, el);
    s->active--;
    if (--s->t_depth == 0) {
        s->t_run_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    }
    if (s->active == 0) {
        embi_view_run_deferred_load(embi_doc_owner(s->doc));
    }
    /* A script replaced the document: the parser must not build into the one that is gone. */
    return s->epoch != epoch ? EMBI_HTML_SCRIPT_STOP : r;
}

bool embi_script_active(const embi_script *s)
{
    return s != nullptr && s->active > 0;
}

static embi_html_script_result on_script_inner(void *ctx, embi_element *el)
{
    embi_script *s = ctx;
    char essence[96];
    script_essence(el, essence, sizeof essence);
    if (SDL_strcmp(essence, "importmap") == 0) {
        run_importmap(s, el);
        return EMBI_HTML_SCRIPT_CONTINUE;
    }
    bool module = SDL_strcmp(essence, "module") == 0;
    if (!module && (!is_js_mime(essence) || has_attr(el, "nomodule"))) {
        return EMBI_HTML_SCRIPT_CONTINUE;
    }
    bool external = has_attr(el, "src");
    bool async = (external || module) && has_attr(el, "async");
    if (module && !external) {
        /* An inline module is deferred like an external one (or async): its text is copied now. */
        queued_script q;
        memset(&q, 0, sizeof q);
        q.module = true;
        q.inline_module = true;
        embi_doc_el_text(el, &q.text);
        if (async) {
            embi_vec_push(&s->asyncs, q);
            embi_loop_task(s->loop, task_async, s);
        } else {
            embi_vec_push(&s->defers, q);
        }
        return EMBI_HTML_SCRIPT_CONTINUE;
    }
    if (external && (async || module || has_attr(el, "defer"))) {
        queued_script q;
        if (queue_script(s, el, module, &q)) {
            if (async) {
                /* The task runs the oldest queued async script, so the scripts run in the order they were queued. */
                embi_vec_push(&s->asyncs, q);
                embi_loop_task(s->loop, task_async, s);
            } else {
                embi_vec_push(&s->defers, q);
            }
        }
        return EMBI_HTML_SCRIPT_CONTINUE;
    }
    /* A classic script blocks the parser. When its source (or a stylesheet before it) is still downloading, the parser pauses
     * instead of the thread: embi_script_run_blocked runs the script when the network has delivered, and the parse goes on. */
    queued_script q;
    memset(&q, 0, sizeof q);
    if (script_must_wait(s, el, external, &q)) {
        s->blk.q = q;
        s->blk.set = true;
        return EMBI_HTML_SCRIPT_PAUSE;
    }
    run_element(s, el);
    return EMBI_HTML_SCRIPT_CONTINUE;
}

bool embi_script_has_blocked(const embi_script *s)
{
    return s->blk.set;
}

bool embi_script_run_blocked(embi_script *s)
{
    if (!s->blk.set) {
        return true;
    }
    if (queued_pending(s, &s->blk.q) || sheets_block_scripts(s)) {
        return false;
    }
    queued_script q = s->blk.q;
    memset(&s->blk, 0, sizeof s->blk);
    s->active++;
    run_queued(s, &q);
    s->active--;
    if (s->active == 0) {
        embi_view_run_deferred_load(embi_doc_owner(s->doc));
    }
    return true;
}

/* ------------------------------------------------------------------ lifecycle events and the loop hooks */

/* Runs the oldest queued async script that is ready (a local one always is; a network one when its bytes are in). With net_only,
 * only a network one: the arrival of a download runs those, in the order the downloads finished. False when none ran. */
static bool run_ready_async(embi_script *s, bool net_only)
{
    for (uint32_t i = 0; i < s->asyncs.len; i++) {
        queued_script *c = &s->asyncs.data[i];
        if ((net_only && !c->net) || queued_pending(s, c)) {
            continue;
        }
        queued_script q = *c;
        memmove(c, c + 1, (size_t)(s->asyncs.len - i - 1) * sizeof *c);
        s->asyncs.len--;
        run_queued(s, &q);
        return true;
    }
    return false;
}

static void task_async(void *user)
{
    (void)run_ready_async(user, false);
}

/* Fires a lifecycle event at its target and runs the microtasks it queued. */
static void fire_lifecycle(embi_script *s, const char *type)
{
    if (s->ctx != nullptr) {
        embi_bind_fire_lifecycle(s->ctx, type);
        embi_js_run_jobs(s->rt);
    }
}

static void task_dom_content_loaded(void *user)
{
    fire_lifecycle(user, "DOMContentLoaded");
}

/* The load task: the document is complete (readystatechange), then load fires at the window. */
static void task_load(void *user)
{
    embi_script *s = user;
    s->ready = SCRIPT_COMPLETE;
    fire_lifecycle(s, "readystatechange");
    fire_lifecycle(s, "load");
    embi_frames_doc_complete(embi_doc_owner(s->doc)); /* IF1: the iframe of a frame gets its load event */
}

void embi_script_parse_done(embi_script *s)
{
    /* The parse is over: the module preloads start, then interactive, before the deferred scripts and DOMContentLoaded. */
    preload_links(s);
    s->ready = SCRIPT_INTERACTIVE;
    fire_lifecycle(s, "readystatechange");
    s->had_defers = s->defers.len > 0;
    s->defer_next = 0;
}

bool embi_script_run_defers(embi_script *s)
{
    const uint32_t epoch = s->epoch;
    while (s->defer_next < s->defers.len || s->mod_wait) {
        if (s->mod_wait) { /* the module before is still importing */
            if (!s->mod_done && s->mods != nullptr && embi_modules_pending(s->mods)) {
                return false;
            }
            s->mod_wait = false;
            continue;
        }
        queued_script *c = &s->defers.data[s->defer_next];
        /* In document order: a script that has not arrived (or a stylesheet before it) holds back the ones after it. */
        if (queued_pending(s, c) || sheets_block_scripts(s)) {
            return false;
        }
        queued_script q = *c;
        memset(c, 0, sizeof *c);
        s->defer_next++;
        s->mod_done = false;
        s->mod_await = 0;
        s->mod_capture = q.module;
        run_queued(s, &q);
        s->mod_capture = false;
        if (s->epoch != epoch) {
            return true; /* a script replaced the document: its caller sees the load changed */
        }
        /* A module whose imports are still downloading holds the scripts after it (its evaluation promise is pending and the
         * engine waits for the host); one that already ran does not wait for a dynamic import() it started. */
        if (q.module && !s->mod_done && s->mods != nullptr && embi_modules_pending(s->mods)) {
            s->mod_wait = true;
        }
    }
    for (uint32_t i = 0; i < s->defers.len; i++) {
        embi_sb_free(&s->defers.data[i].url);
        embi_sb_free(&s->defers.data[i].text);
    }
    embi_vec_clear(&s->defers);
    s->defer_next = 0;
    return true;
}

void embi_script_finish_events(embi_script *s)
{
    /* A body onload attribute is a listener of the window's load event, with no script of its own to make the realm. */
    if (embi_doc_body_has_load_handler(s->doc)) {
        (void)ensure_js(s);
    }
    /* With nothing to listen for, the events are not queued at all: a static page does no work here. */
    if (s->ctx != nullptr || s->had_defers || s->asyncs.len > 0) {
        embi_loop_task(s->loop, task_dom_content_loaded, s);
        /* load waits for the downloads still going on (images, fonts, sheets, async scripts); a document with none, and every
         * local one, fires it right after DOMContentLoaded as before. */
        emb_view *v = embi_doc_owner(s->doc);
        if (v != nullptr && embi_netload_busy(v)) {
            s->load_gated = true;
        } else {
            embi_loop_task(s->loop, task_load, s);
        }
    } else {
        s->ready = SCRIPT_COMPLETE;
        embi_frames_doc_complete(embi_doc_owner(s->doc)); /* IF1: the iframe of a frame gets its load event */
    }
}

void embi_script_net_arrived(embi_script *s, embi_str url)
{
    (void)url;
    const uint32_t epoch = s->epoch;
    /* Imports the engine asked for while they were downloading. */
    if (s->mods != nullptr && embi_modules_pending(s->mods)) {
        embi_modules_retry(s->mods);
        if (s->epoch == epoch && s->rt != nullptr) {
            embi_js_run_jobs(s->rt);
        }
    }
    /* Async scripts run as their bytes arrive, in no order. */
    while (s->epoch == epoch && run_ready_async(s, true)) {
    }
    if (s->epoch == epoch) {
        embi_script_recheck_gate(s);
    }
}

void embi_script_recheck_gate(embi_script *s)
{
    if (s != nullptr && s->load_gated) {
        emb_view *v = embi_doc_owner(s->doc);
        if (v == nullptr || !embi_netload_busy(v)) {
            s->load_gated = false;
            embi_loop_task(s->loop, task_load, s);
        }
    }
}

static void hook_fire(void *user, void *payload)
{
    embi_script *s = user;
    if (s->ctx == nullptr) {
        return;
    }
    embi_js_value r = embi_bind_fire_timer(s->ctx, payload);
    if (r == EMBI_JS_EXCEPTION) {
        report_exception(s);
    }
}

static void hook_release(void *user, void *payload)
{
    embi_script *s = user;
    if (s->ctx != nullptr) {
        embi_bind_release_timer(s->ctx, payload);
    }
}

static void hook_checkpoint(void *user)
{
    embi_script *s = user;
    if (s->rt != nullptr) {
        embi_js_run_jobs(s->rt);
    }
}

/* ------------------------------------------------------------------ the document's script state */

embi_script *embi_script_new(embi_doc *d)
{
    embi_script *s = embi_calloc(1, sizeof *s);
    s->doc = d;
    embi_loop_hooks hooks = {.user = s, .fire = hook_fire, .release = hook_release, .checkpoint = hook_checkpoint};
    s->loop = embi_loop_new(&hooks);
    s->origin_ms = embi_clock_ms();
    s->ready = SCRIPT_COMPLETE;
    pre_start(s);
    return s;
}

/* Frees the copies of the queued scripts from index from. */
static void drop_queue(uint32_t from, queued_script *data, uint32_t len)
{
    for (uint32_t i = from; i < len; i++) {
        embi_sb_free(&data[i].url);
        embi_sb_free(&data[i].text);
    }
}

void embi_script_reset(embi_script *s)
{
    embi_loop_clear_timers(s->loop);
    embi_loop_clear_tasks(s->loop);
    drop_queue(0, s->defers.data, s->defers.len);
    drop_queue(0, s->asyncs.data, s->asyncs.len);
    embi_vec_clear(&s->defers);
    embi_vec_clear(&s->asyncs);
    if (s->blk.set) {
        embi_sb_free(&s->blk.q.url);
        embi_sb_free(&s->blk.q.text);
    }
    memset(&s->blk, 0, sizeof s->blk);
    s->defer_next = 0;
    s->mod_wait = false;
    s->had_defers = false;
    s->load_gated = false;
    s->host_on = false;
    s->epoch++;
    s->t_realm_ms = 0.0;
    s->t_run_ms = 0.0;
    s->ready = SCRIPT_LOADING;
    s->origin_ms = embi_clock_ms();
    if (s->ctx != nullptr) {
        embi_rt_unbind_ctx(s->ctx);
        embi_bind_free(s->ctx);
        s->ctx = nullptr;
    }
    if (s->rt != nullptr) {
        embi_js_runtime_free(s->rt);
        s->rt = nullptr;
    }
    /* The module host goes with the runtime: its records are read by the engine's objects only while the runtime lives. */
    embi_modules_free(s->mods);
    s->mods = nullptr;
}

void embi_script_free(embi_script *s)
{
    if (s == nullptr) {
        return;
    }
    embi_script_reset(s);
    pre_discard(s);
    embi_loop_free(s->loop);
    embi_vec_free(&s->defers);
    embi_vec_free(&s->asyncs);
    embi_free(s, sizeof *s);
}

bool embi_script_pump(embi_script *s)
{
    pre_reap(s);
    return embi_loop_pump(s->loop, embi_clock_ms());
}

void embi_script_set_console(embi_script *s, emb_console_fn fn, void *user)
{
    s->console = fn;
    s->console_user = user;
}

void embi_script_set_log(embi_script *s, void (*fn)(void *user, emb_log_level level, const char *text), void *user)
{
    s->log = fn;
    s->log_user = user;
}

bool embi_script_post(embi_script *s, emb_task_fn fn, void *user)
{
    return embi_loop_post(s->loop, fn, user);
}

bool embi_script_post_ex(embi_script *s, emb_task_fn fn, void *user, void (*drop)(void *user))
{
    return embi_loop_post_ex(s->loop, fn, user, drop);
}

bool embi_script_eval(embi_script *s, const char *src, size_t len, emb_eval_result_fn result, void *user)
{
    if (embi_doc_dom(s->doc) == nullptr) {
        if (result != nullptr) {
            result(user, false, "no document", 11);
        }
        return false;
    }
    if (!ensure_js(s)) {
        if (result != nullptr) {
            result(user, false, "no realm", 8);
        }
        return false;
    }
    embi_js_value v = embi_js_eval(embi_bind_realm(s->ctx), src, len, "eval", 0);
    bool ok = v != EMBI_JS_EXCEPTION;
    embi_sb out = {0};
    if (ok) {
        describe(s, v, &out);
    } else {
        embi_js_value ex = embi_js_exception_get(s->rt);
        embi_js_exception_clear(s->rt);
        describe(s, ex, &out);
    }
    embi_js_run_jobs(s->rt);
    if (result != nullptr) {
        result(user, ok, out.len > 0 ? out.data : "", out.len);
    }
    embi_sb_free(&out);
    return ok;
}

/* ------------------------------------------------------------------ the public API (ember_script.h) */

bool emb_view_eval(emb_view *view, const char *js, size_t len, emb_eval_result_fn result, void *user)
{
    if (view == nullptr || view->doc == nullptr || js == nullptr) {
        return false;
    }
    return embi_script_eval(embi_doc_script(view->doc), js, len, result, user);
}

void emb_view_set_console_callback(emb_view *view, emb_console_fn fn, void *user)
{
    if (view == nullptr || view->doc == nullptr) {
        return;
    }
    embi_script_set_console(embi_doc_script(view->doc), fn, user);
}

bool emb_view_post_task(emb_view *view, emb_task_fn fn, void *user)
{
    if (view == nullptr || view->doc == nullptr || fn == nullptr) {
        return false;
    }
    return embi_script_post(embi_doc_script(view->doc), fn, user);
}

bool embi_view_post_task_ex(emb_view *view, emb_task_fn fn, void *user, void (*drop)(void *user))
{
    if (view == nullptr || view->doc == nullptr || fn == nullptr) {
        return false;
    }
    return embi_script_post_ex(embi_doc_script(view->doc), fn, user, drop);
}

bool embi_script_next_due(embi_script *s, double *due_ms)
{
    return embi_loop_next_due(s->loop, due_ms);
}

bool embi_script_idle(embi_script *s, bool anim_soon)
{
    if (s->ctx == nullptr || s->ready != SCRIPT_COMPLETE || s->active != 0 || embi_loop_has_tasks(s->loop)) {
        return false;
    }
    return embi_bind_idle(s->ctx, anim_soon);
}

bool embi_script_has_tasks(embi_script *s)
{
    return embi_loop_has_tasks(s->loop);
}

void embi_script_set_wake(embi_script *s, bool wake)
{
    embi_loop_set_wake(s->loop, wake);
}

/* ------------------------------------------------------------------ the host channel: messages (HX1) */

/* A message of the host, copied at its post: the bytes follow the header in the same block. */
typedef struct host_msg {
    emb_view *view;
    size_t len;
} host_msg;

static void host_msg_free(host_msg *m)
{
    embi_free(m, sizeof *m + m->len);
}

static void host_msg_drop(void *user)
{
    host_msg_free(user);
}

/* Runs on the view's thread: calls the current document's onMessage with the message, then the microtasks. The message is
 * dropped when that document has no channel or onMessage is not callable. */
static void host_msg_run(void *user)
{
    host_msg *m = user;
    emb_view *v = m->view;
    embi_script *s = v->doc != nullptr ? embi_doc_script(v->doc) : nullptr;
    embi_bind_ctx *c = s != nullptr ? embi_script_bind_if(s) : nullptr;
    if (c != nullptr && s->host_on) {
        embi_js_realm *r = embi_bind_realm(c);
        embi_js_runtime *rt = s->rt;
        embi_js_value chan = embi_js_get(rt, r->global, embi_js_key_c(rt, s->host_name));
        if (chan == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(rt);
        } else if (embi_js_is_object(chan)) {
            embi_js_value fn = embi_js_get_v(rt, chan, embi_js_key_c(rt, "onMessage"));
            if (fn == EMBI_JS_EXCEPTION) {
                report_exception(s);
            } else if (embi_js_is_callable(fn)) {
                embi_js_value arg = embi_js_from_string(embi_js_str_new_utf8(rt, (const char *)(m + 1), m->len));
                if (embi_js_call(rt, fn, chan, 1, &arg) == EMBI_JS_EXCEPTION) {
                    report_exception(s);
                }
            }
        }
        embi_js_run_jobs(rt);
    }
    host_msg_free(m);
}

/* Whether name is an ASCII identifier of at most 63 bytes (a letter, _ or $, then letters, digits, _ or $). */
static bool host_name_ok(const char *name)
{
    if (name == nullptr) {
        return false;
    }
    size_t n = strlen(name);
    if (n == 0 || n > 63) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
        bool digit = c >= '0' && c <= '9';
        if (!letter && !(digit && i > 0)) {
            return false;
        }
    }
    return true;
}

bool emb_view_set_host_channel(emb_view *view, const char *name, emb_host_message_fn fn, void *user)
{
    if (view == nullptr || !host_name_ok(name)) {
        return false;
    }
    memcpy(view->host_name, name, strlen(name) + 1);
    view->host_channel = true;
    view->host_fn = fn;
    view->host_user = user;
    if (view->doc != nullptr) {
        host_channel_install(embi_doc_script(view->doc), view); /* at once when the realm exists (else at its setup) */
    }
    return true;
}

bool emb_view_post_host_message(emb_view *view, const char *message, size_t len)
{
    if (view == nullptr || message == nullptr) {
        return false;
    }
    host_msg *m = embi_alloc(sizeof *m + len);
    m->view = view;
    m->len = len;
    memcpy(m + 1, message, len);
    if (!embi_view_post_task_ex(view, host_msg_run, m, host_msg_drop)) {
        host_msg_free(m); /* the task was not queued, so no drop runs */
        return false;
    }
    return true;
}
