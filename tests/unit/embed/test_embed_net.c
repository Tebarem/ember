/*
 * Remote documents and subresources through an embedded view (N1d): a page loaded from an http or https
 * URL served on 127.0.0.1 (the test servers of tests/unit/net/net_test.h), with a stylesheet, an @import, a web font, an
 * image, and classic, defer, async and module scripts. Checks the pixels and the layout after the late resources arrive,
 * the order of the scripts, render-blocking stylesheets, cookies (HttpOnly hidden from document.cookie), relative URLs,
 * location navigation, the allowlist (a blocked main document and a blocked subresource), a failed connection, and that
 * an embedder handler for http still wins over the network. Every socket is on 127.0.0.1. Without a GPU device, or with
 * EMBER_NET=OFF, the tests skip.
 */
#include "emb_test.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#ifdef EMBER_NET

#include "embed/embed_int.h"
#include "embed/frame.h"
#include "embed/netload.h"
#include "embed/script_int.h"
#include "net/net.h"
#include "unit/net/net_test.h"

#include <ember/ember.h>
#include <ember/ember_resource.h>
#include <ember/ember_script.h>

#include <string.h>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#define VW 200u
/* Time bounds are stretched on a loaded box and under the sanitizers. */
#if defined(EMB_TEST_ASAN) && EMB_TEST_ASAN
#define SLACK 3u
#else
#define SLACK 1u
#endif
#define VH 160u

/* ------------------------------------------------------------------ the site */

#define MAX_SEEN 64

typedef struct site {
    ts_server *srv;
    SDL_Mutex *m;
    char seen[MAX_SEEN][128];
    char cookie_of[MAX_SEEN][256]; /* the Cookie header of the request in seen[i] */
    int nseen;
    int img_delay, font_delay, css_delay;
    int js_delay; /* /sj.js (N1f) */
    SDL_AtomicInt img_done, font_done;
    int other_port;                /* a second server, for the cross-host page */
    char local_script[600];        /* the URL of a local script that /local.html includes */
} site;

static int seen_count(site *s, const char *path)
{
    int n = 0;
    SDL_LockMutex(s->m);
    for (int i = 0; i < s->nseen; i++) {
        if (strcmp(s->seen[i], path) == 0) {
            n++;
        }
    }
    SDL_UnlockMutex(s->m);
    return n;
}

/* The Cookie header the server saw on the last request for path ("" when none or no header). */
static void seen_cookie(site *s, const char *path, char *out, size_t cap)
{
    out[0] = '\0';
    SDL_LockMutex(s->m);
    for (int i = 0; i < s->nseen; i++) {
        if (strcmp(s->seen[i], path) == 0) {
            SDL_snprintf(out, cap, "%s", s->cookie_of[i]);
        }
    }
    SDL_UnlockMutex(s->m);
}

/* A w x h GIF, every pixel red (palette index 0). LZW with a clear code every two pixels keeps the codes at 3 bits. */
static uint8_t *make_gif(uint32_t w, uint32_t h, size_t *len)
{
    uint32_t n = w * h;
    size_t cap = 64 + (size_t)n * 2 + n / 100;
    uint8_t *out = SDL_malloc(cap);
    size_t o = 0;
    memcpy(out + o, "GIF89a", 6);
    o += 6;
    out[o++] = (uint8_t)w;
    out[o++] = (uint8_t)(w >> 8);
    out[o++] = (uint8_t)h;
    out[o++] = (uint8_t)(h >> 8);
    out[o++] = 0x81; /* a global color table of 4 entries */
    out[o++] = 0;
    out[o++] = 0;
    static const uint8_t pal[12] = {255, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0, 0};
    memcpy(out + o, pal, 12);
    o += 12;
    out[o++] = 0x2C;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = (uint8_t)w;
    out[o++] = (uint8_t)(w >> 8);
    out[o++] = (uint8_t)h;
    out[o++] = (uint8_t)(h >> 8);
    out[o++] = 0;
    out[o++] = 2; /* minimum code size */
    /* The codes, 3 bits each, least significant bit first. */
    uint8_t *bits = SDL_calloc(1, (size_t)n * 3 / 2 + 16);
    size_t nbit = 0;
#define PUT(code)                                                                                  \
    do {                                                                                           \
        for (int b_ = 0; b_ < 3; b_++) {                                                           \
            if (((code) >> b_) & 1) {                                                              \
                bits[nbit >> 3] |= (uint8_t)(1u << (nbit & 7));                                    \
            }                                                                                      \
            nbit++;                                                                                \
        }                                                                                          \
    } while (0)
    for (uint32_t i = 0; i < n; i += 2) {
        PUT(4);
        PUT(0);
        if (i + 1 < n) {
            PUT(0);
        }
    }
    PUT(5);
#undef PUT
    size_t nbytes = (nbit + 7) / 8;
    for (size_t pos = 0; pos < nbytes;) {
        size_t chunk = nbytes - pos > 255 ? 255 : nbytes - pos;
        out[o++] = (uint8_t)chunk;
        memcpy(out + o, bits + pos, chunk);
        o += chunk;
        pos += chunk;
    }
    out[o++] = 0;
    out[o++] = 0x3B;
    SDL_free(bits);
    *len = o;
    return out;
}

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8><title>Remote</title>"
    "<link rel=stylesheet href=\"/s.css\"><script src=\"/a.js\"></script></head><body>"
    "<script>window.order.push('inline');document.cookie='b=3';</script>"
    "<div id=box></div><div id=box2></div><img id=im src=\"/red.gif\">"
    "<p id=t>Hamburgefonstiv Hamburgefonstiv</p>"
    "<script src=\"/c.js\"></script>"
    "<script defer src=\"/d.js\"></script>"
    "<script type=module src=\"/m.js\"></script>"
    "<script async src=\"/as.js\"></script>"
    "</body></html>";

static const char CSS[] =
    "@import url(/imp.css);\n"
    "html,body{margin:0}\n"
    "#box{width:100px;height:50px;background:#0000ff}\n"
    "@font-face{font-family:RF;src:url(/f.otf)}\n"
    "#t{display:inline-block;font-family:RF,monospace;font-size:20px;margin:0;white-space:nowrap}\n"
    "#im{display:block}\n";

static void handler(ts_conn *c, const ts_req *req, void *user)
{
    site *s = user;
    char path[160];
    SDL_snprintf(path, sizeof path, "%s", req->target);
    char *q = strchr(path, '?');
    if (q != nullptr) {
        *q = '\0';
    }
    const char *ck = ts_req_header(req, "cookie");
    SDL_LockMutex(s->m);
    if (s->nseen < MAX_SEEN) {
        SDL_snprintf(s->seen[s->nseen], sizeof s->seen[0], "%s", path);
        SDL_snprintf(s->cookie_of[s->nseen], sizeof s->cookie_of[0], "%s", ck != nullptr ? ck : "");
        s->nseen++;
    }
    SDL_UnlockMutex(s->m);

    static const char html[] = "Content-Type: text/html; charset=utf-8\r\n";
    static const char js[] = "Content-Type: text/javascript\r\n";
    if (strcmp(path, "/page.html") == 0) {
        ts_respond_str(c, 200, "Content-Type: text/html; charset=utf-8\r\nSet-Cookie: a=1; Path=/\r\n"
                               "Set-Cookie: h=2; Path=/; HttpOnly\r\n",
                       PAGE);
    } else if (strcmp(path, "/s.css") == 0) {
        if (s->css_delay > 0) {
            SDL_Delay((Uint32)s->css_delay);
        }
        ts_respond_str(c, 200, "Content-Type: text/css\r\n", CSS);
    } else if (strcmp(path, "/imp.css") == 0) {
        ts_respond_str(c, 200, "Content-Type: text/css\r\n", "#box2{width:30px;height:20px;background:#00ff00}");
    } else if (strcmp(path, "/a.js") == 0) {
        ts_respond_str(c, 200, js, "window.order=['a'];");
    } else if (strcmp(path, "/c.js") == 0) {
        ts_respond_str(c, 200, js, "window.order.push('c');window.seenCookie=document.cookie;");
    } else if (strcmp(path, "/d.js") == 0) {
        ts_respond_str(c, 200, js, "window.order.push('defer');");
    } else if (strcmp(path, "/m.js") == 0) {
        ts_respond_str(c, 200, js, "window.order.push('module');");
    } else if (strcmp(path, "/as.js") == 0) {
        ts_respond_str(c, 200, js, "window.order.push('async');");
    } else if (strcmp(path, "/red.gif") == 0 || strcmp(path, "/dir/x.gif") == 0) {
        if (s->img_delay > 0) {
            SDL_Delay((Uint32)s->img_delay);
        }
        size_t n = 0;
        uint8_t *g = make_gif(40, 30, &n);
        ts_respond(c, 200, "Content-Type: image/gif\r\n", g, n);
        SDL_free(g);
        SDL_SetAtomicInt(&s->img_done, 1);
    } else if (strcmp(path, "/f.otf") == 0) {
        if (s->font_delay > 0) {
            SDL_Delay((Uint32)s->font_delay);
        }
        size_t n = 0;
        void *f = SDL_LoadFile(EMBER_SOURCE_DIR "/tests/fonts/SourceSans3-Regular.otf", &n);
        if (f != nullptr) {
            ts_respond(c, 200, "Content-Type: font/otf\r\nAccess-Control-Allow-Origin: *\r\n", f, n);
            SDL_free(f);
        } else {
            ts_respond_str(c, 404, nullptr, "no font");
        }
        SDL_SetAtomicInt(&s->font_done, 1);
    } else if (strcmp(path, "/dl1.css") == 0 || strcmp(path, "/dl2.css") == 0 || strcmp(path, "/dl3.css") == 0) {
        SDL_Delay(300);
        ts_respond_str(c, 200, "Content-Type: text/css\r\n", "p{margin:0}");
    } else if (strcmp(path, "/dl.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>DL</title><link rel=stylesheet href=\"/dl1.css\">"
                       "<link rel=stylesheet href=\"/dl2.css\"><link rel=stylesheet href=\"/dl3.css\"><body><p>x</p>");
    } else if (strcmp(path, "/ff1.otf") == 0 || strcmp(path, "/ff2.otf") == 0 || strcmp(path, "/ff3.otf") == 0) {
        SDL_Delay(path[3] == '1' ? 100 : path[3] == '2' ? 500 : 900);
        size_t n = 0;
        void *f = SDL_LoadFile(EMBER_SOURCE_DIR "/tests/fonts/SourceSans3-Regular.otf", &n);
        if (f != nullptr) {
            ts_respond(c, 200, "Content-Type: font/otf\r\n", f, n);
            SDL_free(f);
        } else {
            ts_respond_str(c, 404, nullptr, "no font");
        }
    } else if (strcmp(path, "/ff.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>FF</title><style>"
                       "@font-face{font-family:A;src:url(/ff1.otf)}@font-face{font-family:B;src:url(/ff2.otf)}"
                       "@font-face{font-family:C;src:url(/ff3.otf)}"
                       "p{font-family:A,B,C,monospace}</style><body><p>Hamburgefonstiv</p>");
    } else if (strcmp(path, "/slow.js") == 0) {
        SDL_Delay(1500);
        ts_respond_str(c, 200, js, "window.slowRan=1;");
    } else if (strcmp(path, "/slow.gif") == 0) {
        SDL_Delay(1200);
        size_t n = 0;
        uint8_t *g = make_gif(8, 8, &n);
        ts_respond(c, 200, "Content-Type: image/gif\r\n", g, n);
        SDL_free(g);
    } else if (strcmp(path, "/slowdoc.html") == 0) {
        SDL_Delay(800);
        ts_respond_str(c, 200, html, "<!doctype html><title>Slow</title><body>slow");
    } else if (strcmp(path, "/slowpage.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>SP</title><body><img src=\"/slow.gif\"><script src=\"/slow.js\"></script>");
    } else if (strcmp(path, "/e404.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>E</title><link rel=stylesheet href=\"/nope.css\">"
                       "<script src=\"/nope.js\"></script><script>window.okay=1</script><body style=\"margin:0\">"
                       "<div style=\"width:50px;height:50px;background:#0000ff\"></div>");
    } else if (strcmp(path, "/redir.html") == 0) {
        ts_respond_str(c, 302, "Location: /dir/b.html\r\n", "");
    } else if (strcmp(path, "/local.html") == 0) {
        char body[900];
        SDL_snprintf(body, sizeof body,
                     "<!doctype html><title>Local</title><img src=\"file:///nothing.gif\" onerror=\"window.fileErr=1\">"
                     "<img src=\"app://x/i.gif\"><script src=\"app://x/a.js\"></script>"
                     "<script src=\"%s\"></script>"
                     "<script src=\"data:text/javascript,window.dataRan=1\"></script>",
                     s->local_script);
        ts_respond_str(c, 200, html, body);
    } else if (strcmp(path, "/ct.html") == 0) {
        ts_respond_str(c, 200, "Content-Type: text/plain\r\n", "<!doctype html><title>Plain text</title>");
    } else if (strcmp(path, "/late.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Late</title><script src=\"/slow.js\"></script>"
                       "<script>window.after=1</script><body>x");
    } else if (strcmp(path, "/imgs.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Imgs</title><body><img src=\"/red.gif\"><img src=\"/red.gif\">");
    } else if (strcmp(path, "/sameface.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>SF</title><style>"
                       "@font-face{font-family:W;font-weight:400;src:url(/ff1.otf)}"
                       "@font-face{font-family:W;font-weight:700;src:url(/ff1.otf)}"
                       "@font-face{font-family:W;font-style:italic;src:url(/ff1.otf)}"
                       "p{font-family:W,monospace}</style><body><p>Hamburgefonstiv</p><p><b>Bold</b></p>");
    } else if (strcmp(path, "/badimg.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Bad</title><body><img id=a src=\"/junk.gif\" "
                       "onload=\"window.la=1\" onerror=\"window.ea=1\">");
    } else if (strcmp(path, "/junk.gif") == 0) {
        ts_respond_str(c, 200, "Content-Type: image/gif\r\n", "this is not an image at all");
    } else if (strcmp(path, "/img.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>I</title><body><img id=a src=\"/red.gif\" "
                       "onload=\"window.la=(window.la||0)+1\" onerror=\"window.ea=1\">"
                       "<img id=b src=\"/nope.gif\" onerror=\"window.eb=1\">");
    } else if (strcmp(path, "/nav1.html") == 0) {
        ts_respond_str(c, 200, html, "<!doctype html><title>N1</title><body><p>one</p>");
    } else if (strcmp(path, "/dir/b.html") == 0) {
        ts_respond_str(c, 200, html, "<!doctype html><title>B</title><body><img src=\"x.gif\"><script src=\"../a.js\"></script>");
    } else if (strcmp(path, "/cross.html") == 0) {
        char body[600];
        SDL_snprintf(body, sizeof body,
                     "<!doctype html><title>Cross</title><link rel=stylesheet href=\"http://127.0.0.1:%d/x.css\">"
                     "<body style=\"margin:0\"><div style=\"width:50px;height:50px;background:#0000ff\"></div>"
                     "<img src=\"http://127.0.0.1:%d/red.gif\">",
                     s->other_port, s->other_port);
        ts_respond_str(c, 200, html, body);
    } else if (strcmp(path, "/plain.html") == 0) {
        ts_respond_str(c, 200, html, "<!doctype html><title>Plain</title><body style=\"margin:0\">"
                                     "<div style=\"width:50px;height:50px;background:#00ff00\"></div>");
    } else if (strcmp(path, "/sj.js") == 0) {
        if (s->js_delay > 0) {
            SDL_Delay((Uint32)s->js_delay);
        }
        ts_respond_str(c, 200, js, "window.sj=1;");
    } else if (strcmp(path, "/blk.html") == 0) {
        /* A parser-blocking script, then an inline one that needs it, then a script-made element and markup after them. */
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Blk</title><body style=\"margin:0\">"
                       "<div style=\"width:50px;height:50px;background:#0000ff\"></div>"
                       "<script src=\"/sj.js\"></script>"
                       "<script>window.after=String(window.sj)+'x';var d=document.createElement('p');d.id='made';"
                       "document.body.appendChild(d);window.dw=typeof document.write;</script>"
                       "<p id=tail>tail</p><p id=tail2>tail2</p>");
    } else if (strcmp(path, "/sheet.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Sh</title><link rel=stylesheet href=\"/ssheet.css\"><body>"
                       "<div id=b></div>");
    } else if (strcmp(path, "/ssheet.css") == 0) {
        if (s->css_delay > 0) {
            SDL_Delay((Uint32)s->css_delay);
        }
        ts_respond_str(c, 200, "Content-Type: text/css\r\n",
                       "body{margin:0}#b{width:50px;height:50px;background:#ff0000}");
    } else if (strcmp(path, "/sheet2.html") == 0) {
        /* A script after the sheet reads what the sheet says: scripts wait for the sheets before them. */
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Sh2</title><link rel=stylesheet href=\"/ssheet.css\"><body>"
                       "<div id=b></div><script>window.bw=document.getElementById('b').getBoundingClientRect().width;"
                       "</script>");
    } else if (strcmp(path, "/never.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Never</title><link rel=stylesheet href=\"/never.css\"><body>"
                       "<div id=b></div><script>window.ran=1</script>");
    } else if (strcmp(path, "/never.css") == 0) {
        SDL_Delay(2500);
        ts_respond_str(c, 200, "Content-Type: text/css\r\n", "body{margin:0}#b{width:50px;height:50px;background:#ff0000}");
    } else if (strcmp(path, "/ord.html") == 0) {
        /* DOMContentLoaded and load: a deferred script, an image that arrives late, an async script. */
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Ord</title><body><script>window.o=[];"
                       "document.addEventListener('DOMContentLoaded',function(){o.push('dcl')});"
                       "window.addEventListener('load',function(){o.push('load')});</script>"
                       "<img src=\"/red.gif\" onload=\"o.push('img')\">"
                       "<script defer src=\"/od.js\"></script><script async src=\"/oa.js\"></script>");
    } else if (strcmp(path, "/od.js") == 0) {
        ts_respond_str(c, 200, js, "o.push('defer');");
    } else if (strcmp(path, "/oa.js") == 0) {
        SDL_Delay(900); /* later than the image of /ord.html (500 ms): load must wait for it too */
        ts_respond_str(c, 200, js, "o.push('async');");
    } else if (strcmp(path, "/rm.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Rm</title><body><script id=s src=\"/sj.js\"></script><p id=t>t</p>");
    } else if (strcmp(path, "/defsheet.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>DS</title><link rel=stylesheet href=\"/ssheet.css\"><body>"
                       "<div id=b></div><script defer src=\"/dw.js\"></script>");
    } else if (strcmp(path, "/dw.js") == 0) {
        ts_respond_str(c, 200, js, "window.bw=document.getElementById('b').getBoundingClientRect().width;");
    } else if (strcmp(path, "/err404.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>E4</title><body><script>window.o=[];"
                       "window.addEventListener('load',function(){o.push('load')});</script>"
                       "<img src=\"/nope.gif\" onerror=\"o.push('err')\">");
    } else if (strcmp(path, "/blk2.html") == 0) {
        /* a blocking script, then one that replaces the document (through the embedder's navigation hook) */
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>B2</title><body><script src=\"/sj.js\"></script>"
                       "<script>location.assign('/nav1.html')</script><p id=after>after</p><script>window.late=1</script>");
    } else if (strcmp(path, "/asy.html") == 0) {
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Asy</title><body><script>window.o=[];</script>"
                       "<script async src=\"/as1.js\"></script><script async src=\"/as2.js\"></script>");
    } else if (strcmp(path, "/fontlate.html") == 0) {
        /* A page whose only download is a late font: load must not wait for it. */
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>FP</title><style>@font-face{font-family:RF;src:url(/f.otf)}p{font-family:RF}</style>"
                       "<body><p>Hamburgefonstiv</p><script>window.o=[];"
                       "window.addEventListener('load',function(){o.push('load')});</script>");
    } else if (strcmp(path, "/dmod.html") == 0) {
        /* A deferred module that imports a slow module, an async module that finishes first, a deferred classic script. */
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>DM</title><body><script>window.o=[];</script>"
                       "<script type=module defer src=\"/dm.js\"></script><script type=module async src=\"/am.js\"></script>"
                       "<script defer src=\"/d2.js\"></script>");
    } else if (strcmp(path, "/dm.js") == 0) {
        ts_respond_str(c, 200, js, "import {x} from '/dep.js'; o.push('dm'+x);");
    } else if (strcmp(path, "/am.js") == 0) {
        ts_respond_str(c, 200, js, "o.push('am');");
    } else if (strcmp(path, "/as1.js") == 0) {
        SDL_Delay(700);
        ts_respond_str(c, 200, js, "o.push('as1');");
    } else if (strcmp(path, "/as2.js") == 0) {
        ts_respond_str(c, 200, js, "o.push('as2');");
    } else if (strcmp(path, "/mod.html") == 0) {
        /* A module whose import is slow, then a deferred script: document order holds, the import is awaited. */
        ts_respond_str(c, 200, html,
                       "<!doctype html><title>Mod</title><body><script>window.o=[];</script>"
                       "<script type=module src=\"/mm.js\"></script><script defer src=\"/d2.js\"></script>");
    } else if (strcmp(path, "/mm.js") == 0) {
        ts_respond_str(c, 200, js, "import {x} from '/dep.js'; o.push('mm'+x);"
                                   "import('/dep2.js').then(function(m){o.push('dyn'+m.y)});");
    } else if (strcmp(path, "/dep.js") == 0) {
        SDL_Delay(300);
        ts_respond_str(c, 200, js, "export const x = 1; o.push('dep');");
    } else if (strcmp(path, "/dep2.js") == 0) {
        SDL_Delay(300);
        ts_respond_str(c, 200, js, "export const y = 2;");
    } else if (strcmp(path, "/d2.js") == 0) {
        ts_respond_str(c, 200, js, "o.push('d2');");
    } else {
        ts_respond_str(c, 404, nullptr, "not found");
    }
}

static site g_site;
static site g_other;

static void site_open(site *s, const char *crt, const char *key)
{
    memset(s, 0, sizeof *s);
    s->m = SDL_CreateMutex();
    s->srv = ts_start(handler, s, crt, key);
}

static void site_close(site *s)
{
    ts_stop(s->srv);
    SDL_DestroyMutex(s->m);
    s->srv = nullptr;
}

static void site_url(site *s, char *out, size_t cap, const char *path)
{
    ts_url(s->srv, out, cap, path);
}

/* ------------------------------------------------------------------ the host */

typedef struct log_cap {
    int warns;
    int gaveup; /* the warnings that say a wait was given up */
    char last[512];
} log_cap;

static log_cap g_log;
static bool g_no_script;

static void log_fn(void *user, emb_log_level level, const char *msg)
{
    (void)user;
    if (level == EMB_LOG_WARN) {
        g_log.warns++;
        if (strstr(msg, "gave up") != nullptr) {
            g_log.gaveup++;
        }
        SDL_snprintf(g_log.last, sizeof g_log.last, "%s", msg);
    }
}

typedef struct env {
    embi_gpu *g;
    SDL_GPUDevice *dev;
    emb_runtime *rt;
    emb_view *v;
} env;

/* allow: patterns for the allowlist (nullptr: none). tls: trust the test CA. */
static bool env_open(env *e, const char *const *allow, bool tls)
{
    memset(e, 0, sizeof *e);
    memset(&g_log, 0, sizeof g_log);
    e->g = embi_gpu_create(nullptr, false);
    if (e->g == nullptr) {
        emb_test_cur_skip = "no GPU device";
        return false;
    }
    e->dev = embi_gpu_device(e->g);
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = log_fn;
    e->rt = emb_runtime_create(&rd);
    if (e->rt == nullptr) {
        return false;
    }
    for (; allow != nullptr && *allow != nullptr; allow++) {
        emb_runtime_net_allow(e->rt, *allow);
    }
    if (tls) {
        size_t len = 0;
        char *ca = ts_read_file("ca.crt", &len);
        emb_runtime_net_add_root(e->rt, ca, len);
        SDL_free(ca);
    }
    emb_view_desc d;
    memset(&d, 0, sizeof d);
    d.size = sizeof d;
    d.mode = EMB_VIEW_OFFSCREEN;
    d.width = VW;
    d.height = VH;
    d.scale = 1.0f;
    d.device = e->dev;
    d.no_script = g_no_script;
    e->v = emb_view_create(e->rt, &d);
    return e->v != nullptr;
}

static void env_close(env *e)
{
    emb_view_destroy(e->v);
    emb_runtime_destroy(e->rt);
    embi_gpu_destroy(e->g);
}

static void render_now(env *e)
{
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(e->dev);
    if (cmd == nullptr) {
        return;
    }
    (void)emb_view_render(e->v, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(e->dev, true, &fence, 1);
        SDL_ReleaseGPUFence(e->dev, fence);
    }
}

static uint8_t *readback(env *e)
{
    size_t bytes = (size_t)VW * VH * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(e->dev, &tbci);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(e->dev);
    uint8_t *out = nullptr;
    if (tb == nullptr || cmd == nullptr) {
        if (tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(e->dev, tb);
        }
        return nullptr;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    memset(&region, 0, sizeof region);
    region.texture = emb_view_texture(e->v);
    region.w = VW;
    region.h = VH;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    memset(&info, 0, sizeof info);
    info.transfer_buffer = tb;
    info.pixels_per_row = VW;
    info.rows_per_layer = VH;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(e->dev, true, &fence, 1);
        SDL_ReleaseGPUFence(e->dev, fence);
        void *mapped = SDL_MapGPUTransferBuffer(e->dev, tb, false);
        if (mapped != nullptr) {
            out = SDL_malloc(bytes);
            SDL_memcpy(out, mapped, bytes);
            SDL_UnmapGPUTransferBuffer(e->dev, tb);
        }
    }
    SDL_ReleaseGPUTransferBuffer(e->dev, tb);
    return out;
}

/* Whether the pixel (x, y) of the last frame is the color. */
static bool pixel_is(env *e, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *px = readback(e);
    if (px == nullptr) {
        return false;
    }
    const uint8_t *p = px + ((size_t)y * VW + x) * 4u;
    bool ok = p[0] == r && p[1] == g && p[2] == b && p[3] == 0xFF;
    SDL_free(px);
    return ok;
}

typedef struct js_out {
    char text[1024];
} js_out;

static void js_cb(void *user, bool ok, const char *text, size_t len)
{
    js_out *o = user;
    SDL_snprintf(o->text, sizeof o->text, "%s%.*s", ok ? "" : "!", (int)len, text);
}

static const char *js_eval(env *e, js_out *o, const char *code)
{
    o->text[0] = '\0';
    (void)emb_view_eval(e->v, code, strlen(code), js_cb, o);
    return o->text;
}

static const char *view_title(env *e)
{
    return emb_view_title(e->v);
}

/* Renders until the document's title is title, up to ms. */
static bool wait_title(env *e, const char *title, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        render_now(e);
        if (strcmp(view_title(e), title) == 0) {
            return true;
        }
        SDL_Delay(3);
    }
    return false;
}

/* Renders until the JS expression evaluates to the string want, up to ms. */
static bool wait_js(env *e, const char *code, const char *want, uint32_t ms)
{
    js_out o;
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        render_now(e);
        if (strcmp(js_eval(e, &o, code), want) == 0) {
            return true;
        }
        SDL_Delay(3);
    }
    return false;
}

/* ------------------------------------------------------------------ the tests */

/* Loads the page and waits for its document. */
static bool page_load(env *e, site *s)
{
    char url[200];
    site_url(s, url, sizeof url, "/page.html");
    EMB_CHECK(emb_view_load_url(e->v, url));
    return wait_title(e, "Remote", 20000);
}

static void page_checks(env *e, site *s)
{
    char url[200];
    site_url(s, url, sizeof url, "/page.html");
    /* Nothing shows until the document and its blocking stylesheets are in: the first frame with the document has the
     * stylesheet applied (the box is blue, the @import's box green) and the scripts have run. */
    EMB_CHECK(pixel_is(e, 5, 5, 0, 0, 255));
    EMB_CHECK(pixel_is(e, 5, 55, 0, 255, 0));
    js_out o;
    /* The parser-blocking scripts ran in order, with the inline script between them; the deferred and the module script
     * followed in document order after the parse; the async script ran whenever its bytes arrived after its tag was parsed
     * (N1f: it is no longer held to the end of the queue, so it may come before the deferred ones). The base URL is the
     * remote URL. */
    EMB_REQUIRE(wait_js(e, "String(window.order.length)", "6", 20000));
    EMB_CHECK_EQ_STR(js_eval(e, &o, "(function(o){var i=function(x){return o.indexOf(x)};"
                                    "return String(i('a')<i('inline')&&i('inline')<i('c')&&i('c')<i('defer')&&"
                                    "i('defer')<i('module')&&i('c')<i('async'))})(window.order)"),
                     "true"); /* the six entries are all there (the length check above), so every index is >= 0 */
    EMB_CHECK(strcmp(js_eval(e, &o, "location.href"), url) == 0);
    EMB_CHECK(strcmp(js_eval(e, &o, "document.baseURI"), url) == 0);
    /* document.cookie: the cookie the response set and the one the inline script wrote, not the HttpOnly one. */
    EMB_CHECK(strcmp(js_eval(e, &o, "document.cookie"), "a=1; b=3") == 0);
    EMB_CHECK(strcmp(js_eval(e, &o, "window.seenCookie"), "a=1; b=3") == 0);
    char ck[256];
    seen_cookie(s, "/a.js", ck, sizeof ck);
    EMB_CHECK(strstr(ck, "a=1") != nullptr && strstr(ck, "h=2") != nullptr); /* the jar sends HttpOnly cookies */
    seen_cookie(s, "/c.js", ck, sizeof ck);
    EMB_CHECK(strstr(ck, "b=3") != nullptr);
    /* The image and the font are late (a delayed server) or at least arrive after the document. */
    uint64_t end = SDL_GetTicks() + 20000;
    while (SDL_GetTicks() < end && !(SDL_GetAtomicInt(&s->img_done) && SDL_GetAtomicInt(&s->font_done))) {
        render_now(e);
        SDL_Delay(3);
    }
    EMB_REQUIRE(SDL_GetAtomicInt(&s->img_done) && SDL_GetAtomicInt(&s->font_done));
    /* The image (40 x 30, red) pushes the paragraph down from 70 to 100 once it has arrived and the page laid out again. */
    EMB_CHECK(wait_js(e, "String(document.getElementById('t').getBoundingClientRect().top)", "100", 20000));
    EMB_CHECK(pixel_is(e, 5, 75, 255, 0, 0));
}

EMB_TEST(remote_page_with_all_subresources_http)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.img_delay = 500;
    g_site.font_delay = 1500;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        /* The text before the font arrives is set in the fallback face: its width changes when the font registers. */
        EMB_REQUIRE(page_load(&e, &g_site));
        js_out o;
        char before[64];
        SDL_snprintf(before, sizeof before, "%s",
                     js_eval(&e, &o, "String(document.getElementById('t').getBoundingClientRect().width)"));
        /* Read after the sample: when the server had not sent the font yet, the width above is the fallback's. A slow
         * (sanitizer) box can get here after the font arrived, and then there is no change to wait for. */
        bool font_late = !SDL_GetAtomicInt(&g_site.font_done);
        if (!SDL_GetAtomicInt(&g_site.img_done)) {
            /* The image has not arrived: the document is laid out without it. */
            EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(document.getElementById('t').getBoundingClientRect().top)"), "70");
            EMB_CHECK(!pixel_is(&e, 5, 75, 255, 0, 0));
        }
        page_checks(&e, &g_site);
        /* The font arrived and the text was laid out again in it (another width). */
        char probe[200];
        SDL_snprintf(probe, sizeof probe, "String(document.getElementById('t').getBoundingClientRect().width != %s)", before);
        if (font_late) {
            EMB_CHECK(wait_js(&e, probe, "true", 20000));
        }
        EMB_CHECK_EQ_INT(embi_fonts_face_count(e.rt->fonts), 2);
        EMB_CHECK_EQ_INT(g_log.warns, 0);
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/page.html"), 1);
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/red.gif"), 1);
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/f.otf"), 1);
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/imp.css"), 1);
    }
    if (e.v != nullptr) {
        env_close(&e);
    }
    site_close(&g_site);
}

EMB_TEST(remote_page_with_all_subresources_https)
{
    site_open(&g_site, "server.crt", "server.key");
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, true)) {
        EMB_REQUIRE(page_load(&e, &g_site));
        page_checks(&e, &g_site);
        EMB_CHECK_EQ_INT(g_log.warns, 0);
    }
    if (e.v != nullptr) {
        env_close(&e);
    }
    site_close(&g_site);
}

EMB_TEST(stylesheets_block_the_first_paint)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.css_delay = 400;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/page.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t t0 = SDL_GetTicks();
        bool seen_doc = false;
        while (SDL_GetTicks() - t0 < 20000) {
            render_now(&e);
            if (view_title(&e)[0] != '\0') {
                seen_doc = true;
                break;
            }
            /* Until the document is in, the view shows nothing of it: the canvas is blank (white). */
            EMB_CHECK(pixel_is(&e, 5, 5, 255, 255, 255));
            SDL_Delay(3);
        }
        EMB_REQUIRE(seen_doc);
        EMB_CHECK(SDL_GetTicks() - t0 >= 300);
        EMB_CHECK(pixel_is(&e, 5, 5, 0, 0, 255)); /* the first frame of the page is styled */
        env_close(&e);
    }
    site_close(&g_site);
}

EMB_TEST(location_navigation_and_relative_urls)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/nav1.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "N1", 20000));
        js_out o;
        js_eval(&e, &o, "location.assign('/dir/b.html')");
        EMB_REQUIRE(wait_title(&e, "B", 20000));
        site_url(&g_site, url, sizeof url, "/dir/b.html");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "location.href"), url);
        /* x.gif resolves against /dir/ and ../a.js against it too. */
        for (int i = 0; i < 4000 && seen_count(&g_site, "/dir/x.gif") == 0; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/dir/x.gif"), 1);
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/a.js"), 1);
        /* An assignment of location.href. */
        js_eval(&e, &o, "location.href = '/nav1.html'");
        EMB_REQUIRE(wait_title(&e, "N1", 20000));
        EMB_CHECK_EQ_INT(g_log.warns, 0);
        env_close(&e);
    }
    site_close(&g_site);
}

EMB_TEST(blocked_main_document_shows_an_error_and_warns_once)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    if (env_open(&e, nullptr, false)) { /* nothing allowed */
        char url[200];
        site_url(&g_site, url, sizeof url, "/plain.html");
        EMB_CHECK(emb_view_load_url(e.v, url));
        EMB_CHECK(wait_title(&e, "Cannot load page", 20000));
        EMB_CHECK_EQ_INT(g_log.warns, 1);
        EMB_CHECK(strstr(g_log.last, "127.0.0.1") != nullptr);
        EMB_CHECK_EQ_INT(g_site.nseen, 0);
        env_close(&e);
    }
    site_close(&g_site);
}

EMB_TEST(failed_connection_shows_an_error_and_warns_once)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    char url[200];
    site_url(&g_site, url, sizeof url, "/plain.html");
    site_close(&g_site); /* nothing listens on the port any more */
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        EMB_CHECK(emb_view_load_url(e.v, url));
        EMB_CHECK(wait_title(&e, "Cannot load page", 20000));
        EMB_CHECK_EQ_INT(g_log.warns, 1);
        env_close(&e);
    }
}

EMB_TEST(blocked_subresources_degrade_cleanly)
{
    site_open(&g_site, nullptr, nullptr);
    site_open(&g_other, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr && g_other.srv != nullptr);
    g_site.other_port = g_other.srv->port;
    env e;
    char allow_one[64];
    SDL_snprintf(allow_one, sizeof allow_one, "127.0.0.1:%d", g_site.srv->port);
    const char *allow[] = {allow_one, nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/cross.html");
        EMB_CHECK(emb_view_load_url(e.v, url));
        EMB_CHECK(wait_title(&e, "Cross", 20000));
        for (int i = 0; i < 40; i++) {
            render_now(&e);
            SDL_Delay(5);
        }
        EMB_CHECK(pixel_is(&e, 5, 5, 0, 0, 255)); /* the page itself renders */
        EMB_CHECK_EQ_INT(g_other.nseen, 0);         /* nothing reached the other server */
        EMB_CHECK_EQ_INT(g_log.warns, 2);           /* one line per blocked request: the sheet and the image */
        EMB_CHECK(strstr(g_log.last, "127.0.0.1") != nullptr);
        env_close(&e);
    }
    site_close(&g_other);
    site_close(&g_site);
}

static int handler_calls;

static bool http_handler(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    (void)req;
    static const char body[] = "<!doctype html><title>Mocked</title><body>mock";
    handler_calls++;
    out->status = 200;
    out->mime = "text/html";
    out->mime_len = 9;
    out->data = (const uint8_t *)body;
    out->len = sizeof body - 1;
    return true;
}

EMB_TEST(an_embedder_handler_for_http_wins_over_the_network)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    handler_calls = 0;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        EMB_REQUIRE(emb_runtime_set_resource_handler_v2(e.rt, "http", http_handler, nullptr));
        char url[200];
        site_url(&g_site, url, sizeof url, "/plain.html");
        EMB_CHECK(emb_view_load_url(e.v, url));
        EMB_CHECK(strcmp(view_title(&e), "Mocked") == 0); /* loaded at once, not from the queue */
        EMB_CHECK(handler_calls >= 1);
        EMB_CHECK_EQ_INT(g_site.nseen, 0);
        env_close(&e);
    }
    site_close(&g_site);
}

/* ------------------------------------------------------------------ the N1d review round */

EMB_TEST(link_stylesheets_download_in_parallel)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/dl.html");
        uint64_t t0 = SDL_GetTicks();
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "DL", 20000));
        uint64_t took = SDL_GetTicks() - t0;
        EMB_CHECK(took >= 280);
        EMB_CHECK(took < 850); /* three sheets of 300 ms each in parallel; one after the other is 900 and more */
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(font_faces_arriving_one_by_one_register_once_each)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        uint32_t before = embi_fonts_face_count(e.rt->fonts);
        char url[200];
        site_url(&g_site, url, sizeof url, "/ff.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "FF", 20000));
        uint64_t end = SDL_GetTicks() + 1800;
        while (SDL_GetTicks() < end && embi_fonts_face_count(e.rt->fonts) < before + 3) {
            render_now(&e);
            SDL_Delay(3);
        }
        for (int i = 0; i < 40; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_INT((int)embi_fonts_face_count(e.rt->fonts), (int)before + 3);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(destroying_the_view_during_downloads_leaks_nothing)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        /* during the main document's request */
        site_url(&g_site, url, sizeof url, "/slowdoc.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        SDL_Delay(100);
        env_close(&e);
    }
    if (env_open(&e, allow, false)) {
        char url[200];
        /* during an image download, after the document is in */
        site_url(&g_site, url, sizeof url, "/slowpage.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(navigating_away_during_a_slow_script_leaks_nothing)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/slowpage.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "SP", 20000));
        site_url(&g_site, url, sizeof url, "/nav1.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "N1", 20000));
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(load_html_replaces_a_document_that_is_still_arriving)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/slowdoc.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        static const char html[] = "<!doctype html><title>H</title><body>mine";
        EMB_REQUIRE(emb_view_load_html(e.v, html, sizeof html - 1, nullptr));
        EMB_CHECK_EQ_STR(view_title(&e), "H");
        uint64_t end = SDL_GetTicks() + 1300;
        while (SDL_GetTicks() < end) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_STR(view_title(&e), "H"); /* the late response did not replace it */
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(failed_script_and_sheet_are_skipped_and_the_page_goes_on)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/e404.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "E", 20000));
        render_now(&e);
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.okay)"), "1");
        EMB_CHECK(pixel_is(&e, 5, 5, 0, 0, 255));
        EMB_CHECK(g_log.warns >= 1);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_redirect_final_url_is_the_base_for_relative_urls)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/redir.html#frag");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "B", 20000));
        for (int i = 0; i < 4000 && seen_count(&g_site, "/dir/x.gif") == 0; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/dir/x.gif"), 1);
        js_out o;
        char want[200];
        site_url(&g_site, want, sizeof want, "/dir/b.html#frag");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "location.href"), want); /* final URL, original fragment */
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(network_images_fire_load_and_error_and_set_complete)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.img_delay = 400;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/img.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "I", 20000));
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(document.getElementById('a').complete)"), "false");
        EMB_CHECK(wait_js(&e, "String(window.la)", "1", 10000));
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(document.getElementById('a').complete)"), "true");
        EMB_CHECK(wait_js(&e, "String(window.eb)", "1", 10000));
        for (int i = 0; i < 30; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.la)"), "1"); /* once */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.ea)"), "undefined");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

/* ------------------------------------------------------------------ the second review round */

static int app_calls;

static bool app_handler(void *user, const emb_request_info *req, emb_response *out)
{
    (void)user;
    static const char page[] = "<!doctype html><title>App</title><body><script src=\"app://t/s.js\"></script>";
    static const char js[] = "window.appRan=1;";
    app_calls++;
    out->status = 200;
    if (req->url_len >= 3 && memcmp(req->url + req->url_len - 3, ".js", 3) == 0) {
        out->mime = "text/javascript";
        out->mime_len = 15;
        out->data = (const uint8_t *)js;
        out->len = sizeof js - 1;
    } else {
        out->mime = "text/html";
        out->mime_len = 9;
        out->data = (const uint8_t *)page;
        out->len = sizeof page - 1;
    }
    return true;
}

EMB_TEST(a_remote_page_cannot_reach_local_schemes_and_an_app_page_can)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    /* a real local script: it must not run from a remote page */
    char *dir = SDL_strdup(SDL_GetCurrentDirectory());
    char fpath[600];
    SDL_snprintf(fpath, sizeof fpath, "%slocal_ran_n1d.js", dir);
    SDL_free(dir);
    for (char *p = fpath; *p != '\0'; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    EMB_REQUIRE(SDL_SaveFile(fpath, "window.fileRan=1;", 17));
    SDL_snprintf(g_site.local_script, sizeof g_site.local_script, "file://%s%s", fpath[0] == '/' ? "" : "/", fpath);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        app_calls = 0;
        EMB_REQUIRE(emb_runtime_set_resource_handler_v2(e.rt, "app", app_handler, nullptr));
        char url[200];
        site_url(&g_site, url, sizeof url, "/local.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Local", 20000));
        for (int i = 0; i < 30; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.dataRan)"), "1");   /* data: stays allowed */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.fileRan)"), "undefined"); /* the real file did not run */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.appRan)"), "undefined");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.fileErr)"), "1"); /* the refused image fired error */
        EMB_CHECK_EQ_INT(app_calls, 0); /* the embedder's handler was never asked */
        EMB_CHECK(g_log.warns >= 1);
        EMB_CHECK(strstr(g_log.last, "local resources") != nullptr);
        /* navigation to a local scheme is refused too */
        int warns = g_log.warns;
        (void)js_eval(&e, &o, "location.assign('app://x/p.html')");
        for (int i = 0; i < 30; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_STR(view_title(&e), "Local");
        EMB_CHECK(g_log.warns > warns);
        EMB_CHECK_EQ_INT(app_calls, 0);
        /* and so is fetch() (XMLHttpRequest goes through it) */
        (void)js_eval(&e, &o, "window.fr='';fetch('app://t/index.html').then(()=>{window.fr='ok'},()=>{window.fr='err'})");
        for (int i = 0; i < 200 && strcmp(js_eval(&e, &o, "window.fr"), "") == 0; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "window.fr"), "err");
        EMB_CHECK_EQ_INT(app_calls, 0);
        /* an app:// page loads its own app:// resources */
        EMB_REQUIRE(emb_view_load_url(e.v, "app://t/index.html"));
        EMB_CHECK_EQ_STR(view_title(&e), "App");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.appRan)"), "1");
        (void)js_eval(&e, &o, "window.fr='';fetch('app://t/index.html').then(()=>{window.fr='ok'},()=>{window.fr='err'})");
        for (int i = 0; i < 200 && strcmp(js_eval(&e, &o, "window.fr"), "") == 0; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "window.fr"), "ok"); /* an app:// page fetches app:// */
        env_close(&e);
    }
    (void)SDL_RemovePath(fpath);
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_non_html_main_document_shows_the_error_page)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/ct.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_CHECK(wait_title(&e, "Cannot load page", 20000));
        EMB_CHECK_EQ_INT(g_log.warns, 1);
        EMB_CHECK(strstr(g_log.last, "text/plain") != nullptr);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_blocking_wait_gives_up_and_the_script_is_skipped)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        embi_netload_set_wait_ms(500);
        char url[200];
        site_url(&g_site, url, sizeof url, "/late.html"); /* /slow.js answers after 1500 ms */
        uint64_t t0 = SDL_GetTicks();
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Late", 20000));
        EMB_CHECK(SDL_GetTicks() - t0 < 1400);
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.after)"), "1");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.slowRan)"), "undefined");
        EMB_CHECK_EQ_INT(g_log.gaveup, 1);
        embi_netload_set_wait_ms(0);
        env_close(&e);
    }
    embi_netload_set_wait_ms(0);
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

/* Runs the document's tasks without rendering (a render makes the realm for the observers). True once the title is want. */
static bool pump_title(env *e, const char *want, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        if (e->v->doc != nullptr) {
            (void)embi_script_pump(embi_doc_script(e->v->doc));
            if (strcmp(view_title(e), want) == 0) {
                return true;
            }
        }
        SDL_Delay(3);
    }
    return false;
}

static void pump_for(env *e, uint32_t ms)
{
    uint64_t end = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < end) {
        if (e->v->doc != nullptr) {
            (void)embi_script_pump(embi_doc_script(e->v->doc));
        }
        SDL_Delay(3);
    }
}

EMB_TEST(static_pages_and_scripting_off_views_make_no_realm_for_images)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.img_delay = 150;
    const char *allow[] = {"127.0.0.1", nullptr};
    char url[200];
    env e;
    if (env_open(&e, allow, false)) { /* images, no scripts, no handlers: no realm */
        site_url(&g_site, url, sizeof url, "/imgs.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(pump_title(&e, "Imgs", 20000));
        pump_for(&e, 700);
        EMB_CHECK(seen_count(&g_site, "/red.gif") >= 1); /* the image did arrive, so the arrival path ran */
        EMB_CHECK(embi_script_bind_if(embi_doc_script(e.v->doc)) == nullptr);
        env_close(&e);
    }
    g_no_script = true;
    SDL_SetAtomicInt(&g_site.img_done, 0);
    if (env_open(&e, allow, false)) { /* scripting off: the onload attributes do not run, and no realm */
        site_url(&g_site, url, sizeof url, "/img.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(pump_title(&e, "I", 20000));
        pump_for(&e, 700);
        EMB_CHECK(embi_script_bind_if(embi_doc_script(e.v->doc)) == nullptr);
        env_close(&e);
    }
    g_no_script = false;
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(one_font_file_for_several_rules_is_downloaded_once)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        uint32_t before = embi_fonts_face_count(e.rt->fonts);
        char url[200];
        site_url(&g_site, url, sizeof url, "/sameface.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "SF", 20000));
        uint64_t end = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < end && embi_fonts_face_count(e.rt->fonts) < before + 3) {
            render_now(&e);
            SDL_Delay(3);
        }
        for (int i = 0; i < 40; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_INT((int)embi_fonts_face_count(e.rt->fonts), (int)before + 3);
        EMB_CHECK_EQ_INT(seen_count(&g_site, "/ff1.otf"), 1);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(an_image_that_does_not_decode_fires_error)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/badimg.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Bad", 20000));
        EMB_CHECK(wait_js(&e, "String(window.ea)", "1", 10000));
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.la)"), "undefined");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}


/* ------------------------------------------------------------------ N1f: the resumable load */

/* The view keeps working while a parser-blocking script downloads: the thread is not asleep on it. */
static bool responsive_for(env *e, uint32_t ms, uint32_t *worst)
{
    uint64_t end = SDL_GetTicks() + ms;
    *worst = 0;
    while (SDL_GetTicks() < end) {
        uint64_t t0 = SDL_GetTicks();
        emb_view_mouse_move(e->v, 10.0f, 10.0f, 0);
        emb_view_wheel(e->v, 10.0f, 10.0f, 0.0f, 20.0f);
        render_now(e);
        uint32_t took = (uint32_t)(SDL_GetTicks() - t0);
        if (took > *worst) {
            *worst = took;
        }
        SDL_Delay(5);
    }
    return true;
}

EMB_TEST(a_parser_blocking_script_does_not_block_the_view)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.js_delay = 2000;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/plain.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Plain", 20000));
        render_now(&e);
        EMB_REQUIRE(pixel_is(&e, 5, 5, 0, 255, 0));
        site_url(&g_site, url, sizeof url, "/blk.html");
        uint64_t t0 = SDL_GetTicks();
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        /* The response arrives at once; the parse then waits on /sj.js (2 s) without sleeping. */
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        uint32_t worst = 0;
        (void)responsive_for(&e, 1200, &worst);
        EMB_CHECK(worst < 250 * SLACK);
        EMB_CHECK(embi_doc_load_suspended(e.v->doc));
        /* The previous content stays on screen and is the title the view reports; the half-built page paints nothing. */
        EMB_CHECK(pixel_is(&e, 5, 5, 0, 255, 0));
        EMB_CHECK_EQ_STR(view_title(&e), "Plain");
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "document.readyState"), "loading");
        /* The script arrives, runs before the inline script that needs it, and the parse goes on to the markup after it. */
        EMB_REQUIRE(wait_title(&e, "Blk", 20000));
        EMB_CHECK(SDL_GetTicks() - t0 >= 1800);
        render_now(&e);
        EMB_CHECK(pixel_is(&e, 5, 5, 0, 0, 255));
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "window.after"), "1x");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(!!document.getElementById('made'))"), "true");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(!!document.getElementById('tail2'))"), "true");
        /* The order of the tree is the order of the markup. */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "document.body.lastElementChild.id + ',' + document.getElementById('made').nextElementSibling"
                                         ".id"),
                         "tail2,tail");
        /* document.write is not part of the engine; the script that asked is not disturbed by it. */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "window.dw"), "undefined");
        EMB_CHECK(wait_js(&e, "document.readyState", "complete", 5000));
        EMB_CHECK_EQ_INT(g_log.warns, 0);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_render_blocking_sheet_holds_the_first_paint_without_blocking_the_view)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.css_delay = 800;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/plain.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Plain", 20000));
        render_now(&e);
        EMB_REQUIRE(pixel_is(&e, 5, 5, 0, 255, 0));
        site_url(&g_site, url, sizeof url, "/sheet.html");
        uint64_t t0 = SDL_GetTicks();
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        /* Until the sheet is in, nothing of the new page is painted: the screen is the old page, not the unstyled new one. */
        uint32_t worst = 0;
        uint32_t frames_green = 0;
        while (SDL_GetTicks() - t0 < 600) {
            uint64_t f0 = SDL_GetTicks();
            emb_view_mouse_move(e.v, 10.0f, 10.0f, 0);
            render_now(&e);
            if ((uint32_t)(SDL_GetTicks() - f0) > worst) {
                worst = (uint32_t)(SDL_GetTicks() - f0);
            }
            if (pixel_is(&e, 5, 5, 0, 255, 0)) {
                frames_green++;
            } else {
                EMB_CHECK(false);
                break;
            }
            SDL_Delay(5);
        }
        EMB_CHECK(frames_green > 5);
        EMB_CHECK(worst < 250 * SLACK);
        EMB_REQUIRE(wait_title(&e, "Sh", 20000));
        EMB_CHECK(SDL_GetTicks() - t0 >= 700);
        EMB_CHECK(pixel_is(&e, 5, 5, 255, 0, 0)); /* the first frame of the page is the styled one */
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_script_waits_for_the_stylesheet_before_it)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.css_delay = 300;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/sheet2.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Sh2", 20000));
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.bw)"), "50"); /* the script saw the sheet's width */
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_sheet_that_never_arrives_is_ignored_after_the_timeout)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        embi_netload_set_wait_ms(400);
        char url[200];
        site_url(&g_site, url, sizeof url, "/never.html"); /* /never.css answers after 2.5 s */
        uint64_t t0 = SDL_GetTicks();
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Never", 20000));
        uint64_t took = SDL_GetTicks() - t0;
        EMB_CHECK(took >= 350 && took < 4000);
        render_now(&e);
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.ran)"), "1"); /* the script after the sheet ran after the timeout */
        EMB_CHECK(pixel_is(&e, 5, 5, 255, 255, 255));                  /* the sheet was ignored: the div has no size */
        EMB_CHECK_EQ_INT(g_log.gaveup, 1);
        embi_netload_set_wait_ms(0);
        env_close(&e);
    }
    embi_netload_set_wait_ms(0);
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_script_that_never_arrives_is_skipped_and_the_parse_goes_on)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.js_delay = 2500;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        embi_netload_set_wait_ms(400);
        char url[200];
        site_url(&g_site, url, sizeof url, "/blk.html");
        uint64_t t0 = SDL_GetTicks();
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Blk", 20000));
        uint64_t took = SDL_GetTicks() - t0;
        EMB_CHECK(took >= 350 && took < 4000);
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "window.after"), "undefinedx"); /* the later script ran without it */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(!!document.getElementById('tail2'))"), "true");
        EMB_CHECK_EQ_INT(g_log.gaveup, 1);
        embi_netload_set_wait_ms(0);
        env_close(&e);
    }
    embi_netload_set_wait_ms(0);
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(destroying_the_view_while_the_parse_is_suspended)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.js_delay = 1500;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/blk.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        env_close(&e); /* the parser, its input and the records go with the document; the late arrival has nobody to tell */
    }
    /* and again, suspended on a sheet */
    g_site.css_delay = 1000;
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/sheet.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        env_close(&e);
    }
    SDL_Delay(300);
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(navigating_away_while_the_parse_is_suspended)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.js_delay = 1200;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/blk.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        /* another URL replaces the half-built page */
        site_url(&g_site, url, sizeof url, "/nav1.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "N1", 20000));
        EMB_CHECK(!embi_doc_load_suspended(e.v->doc));
        /* the script that was awaited arrives into nothing: the new page is not disturbed */
        uint64_t end = SDL_GetTicks() + 1600;
        while (SDL_GetTicks() < end) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_STR(view_title(&e), "N1");
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.sj)"), "undefined");
        /* and emb_view_load_html replaces a suspended page too */
        site_url(&g_site, url, sizeof url, "/blk.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        static const char html[] = "<!doctype html><title>H</title><body>mine";
        EMB_REQUIRE(emb_view_load_html(e.v, html, sizeof html - 1, nullptr));
        EMB_CHECK(!embi_doc_load_suspended(e.v->doc));
        end = SDL_GetTicks() + 1600;
        while (SDL_GetTicks() < end) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK_EQ_STR(view_title(&e), "H");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(domcontentloaded_and_load_fire_at_the_right_points)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.img_delay = 500;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/ord.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Ord", 20000));
        js_out o;
        EMB_REQUIRE(wait_js(&e, "String(o.indexOf('load') >= 0)", "true", 20000));
        const char *order = js_eval(&e, &o, "o.join(',')");
        /* The deferred script ran before DOMContentLoaded; load came last: after the image, which was still downloading when
         * DOMContentLoaded fired, and after the async script. */
        char *copy = SDL_strdup(order);
        EMB_CHECK(strstr(copy, "defer") != nullptr && strstr(copy, "dcl") != nullptr && strstr(copy, "img") != nullptr &&
                  strstr(copy, "async") != nullptr);
        EMB_CHECK(strstr(copy, "defer") < strstr(copy, "dcl"));
        EMB_CHECK(strstr(copy, "dcl") < strstr(copy, "img"));
        EMB_CHECK(strstr(copy, "img") < strstr(copy, "async")); /* the async script is the slowest download */
        EMB_CHECK(strstr(copy, "img") < strstr(copy, "load"));
        EMB_CHECK(strstr(copy, "async") < strstr(copy, "load"));
        EMB_CHECK(strstr(copy, "load") != nullptr && strcmp(copy + strlen(copy) - 4, "load") == 0);
        SDL_free(copy);
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "document.readyState"), "complete");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_local_page_fires_domcontentloaded_and_load_as_before)
{
    env e;
    if (env_open(&e, nullptr, false)) {
        static const char html[] = "<!doctype html><title>Loc</title><body><script>window.o=[];"
                                   "document.addEventListener('DOMContentLoaded',function(){o.push('dcl')});"
                                   "window.addEventListener('load',function(){o.push('load')});</script>"
                                   "<script defer src=\"data:text/javascript,o.push('defer')\"></script>";
        EMB_REQUIRE(emb_view_load_html(&*e.v, html, sizeof html - 1, "app://x/index.html"));
        EMB_CHECK(!embi_doc_load_suspended(e.v->doc));
        render_now(&e);
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "o.join(',')"), "defer,dcl,load");
        env_close(&e);
    }
}

EMB_TEST(async_scripts_run_as_each_arrives)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/asy.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "Asy", 20000));
        EMB_REQUIRE(wait_js(&e, "String(o.length)", "2", 20000));
        js_out o;
        /* as1 is the first in the markup and the last to arrive (700 ms): the fast one does not wait for it. */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "o.join(',')"), "as2,as1");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_module_graph_completes_from_arrivals_and_runs_in_order_with_defer)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/mod.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t t0 = SDL_GetTicks();
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        /* While the import downloads (300 ms) the view keeps running. */
        uint32_t worst = 0;
        (void)responsive_for(&e, 150, &worst);
        EMB_CHECK(worst < 250 * SLACK);
        EMB_REQUIRE(wait_js(&e, "String(o.length >= 4)", "true", 20000));
        js_out o;
        /* The module's dependency, the module, then the deferred script that follows it in the markup; the dynamic import() of
         * a module still downloading settles later, from its arrival. */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "o.join(',')"), "dep,mm1,d2,dyn2");
        EMB_CHECK(SDL_GetTicks() - t0 >= 280);
        EMB_CHECK_EQ_INT(g_log.warns, 0);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}


/* ------------------------------------------------------------------ N1f review round */

static void replace_hook(void *user, const char *url, size_t len)
{
    (void)url;
    (void)len;
    emb_view *v = user;
    static const char html[] = "<!doctype html><title>Rep</title><body>replaced";
    (void)emb_view_load_html(v, html, sizeof html - 1, nullptr);
}

EMB_TEST(a_script_that_replaces_the_document_during_the_parse_and_the_resume)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.js_delay = 300;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        emb_view_set_navigate_handler(e.v, replace_hook, e.v);
        /* during the parse of a local page (the parser is on the stack, the input is the caller's buffer) */
        static const char local[] = "<!doctype html><title>L</title><body><script>location.assign('app://x/n')</script>"
                                    "<p id=after>after</p><script>window.late=1</script><p>more</p>";
        EMB_REQUIRE(emb_view_load_html(e.v, local, sizeof local - 1, "app://x/index.html"));
        EMB_CHECK_EQ_STR(view_title(&e), "Rep");
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.late)"), "undefined");
        /* during a resume: the parser was suspended on /sj.js, the script after it replaces the document */
        char url[200];
        site_url(&g_site, url, sizeof url, "/blk2.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        until = SDL_GetTicks() + 20000;
        while (SDL_GetTicks() < until && embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        for (int i = 0; i < 20; i++) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_CHECK(!embi_doc_load_suspended(e.v->doc));
        EMB_CHECK_EQ_STR(view_title(&e), "Rep");
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.late)"), "undefined");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(removing_the_blocked_script_element_while_suspended)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.js_delay = 1000;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/rm.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        uint64_t until = SDL_GetTicks() + 3000;
        while (SDL_GetTicks() < until && !embi_doc_load_suspended(e.v->doc)) {
            render_now(&e);
            SDL_Delay(3);
        }
        EMB_REQUIRE(embi_doc_load_suspended(e.v->doc));
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "document.getElementById('s').remove(); String(document.getElementById('s'))"), "null");
        EMB_REQUIRE(wait_title(&e, "Rm", 20000));
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.sj)"), "1"); /* the script was already prepared: it ran */
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(!!document.getElementById('t'))"), "true");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_deferred_script_waits_for_the_sheet_before_it)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.css_delay = 400;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/defsheet.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "DS", 20000));
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "String(window.bw)"), "50");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(a_failing_image_does_not_hold_the_load_event)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/err404.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_title(&e, "E4", 20000));
        EMB_REQUIRE(wait_js(&e, "String(o.indexOf('load') >= 0)", "true", 20000));
        js_out o;
        EMB_CHECK_EQ_STR(js_eval(&e, &o, "o.join(',')"), "err,load");
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

/* ------------------------------------------------------------------ N1f review round 3 */

EMB_TEST(a_late_font_does_not_hold_the_load_event)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    g_site.font_delay = 2500;
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/fontlate.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_js(&e, "String(o.indexOf('load') >= 0)", "true", 20000));
        /* The font handler sleeps 2.5 s before it answers: load came while it was still asleep. */
        EMB_CHECK_EQ_INT(SDL_GetAtomicInt(&g_site.font_done), 0);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

EMB_TEST(an_async_module_finishing_does_not_release_a_deferred_module_waiting_for_its_import)
{
    site_open(&g_site, nullptr, nullptr);
    EMB_REQUIRE(g_site.srv != nullptr);
    env e;
    const char *allow[] = {"127.0.0.1", nullptr};
    if (env_open(&e, allow, false)) {
        char url[200];
        site_url(&g_site, url, sizeof url, "/dmod.html");
        EMB_REQUIRE(emb_view_load_url(e.v, url));
        EMB_REQUIRE(wait_js(&e, "String(o.length >= 4)", "true", 20000));
        js_out o;
        const char *order = js_eval(&e, &o, "o.join(',')");
        char *copy = SDL_strdup(order);
        EMB_CHECK(strstr(copy, "dm1") != nullptr && strstr(copy, "d2") != nullptr && strstr(copy, "am") != nullptr);
        EMB_CHECK(strstr(copy, "dm1") < strstr(copy, "d2")); /* document order: the deferred module, then the script after it */
        SDL_free(copy);
        env_close(&e);
    }
    site_close(&g_site);
    EMB_CHECK_EQ_INT((int)embi_net_live(), 0);
}

#else

EMB_TEST(skipped_without_network)
{
    EMB_SKIP("EMBER_NET is off");
}

#endif

/* The GPU tests use the video subsystem, initialized once around the run (the offscreen driver unless the environment names
 * one, so they run on a machine without a display). */
int main(void)
{
    (void)SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_VIDEO_DRIVER", "offscreen", false);
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
