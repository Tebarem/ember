/*
 * iframes (IF1): a frame's document renders in its iframe's content box (clipped, transparent where it
 * paints nothing, under the parent's elements painted above it), fires load at its iframe, is reached only through remote
 * window proxies and postMessage, takes the input routed to it, reloads on src, goes away with its iframe, and nests.
 * Every test needs an offscreen GPU device; without one they skip.
 */
#include "input_rig.h"

#include "embed/frames.h"
#include "embed/url.h"

#include <SDL3/SDL_timer.h>

static bool render_now(emb_view *v)
{
    SDL_GPUDevice *dev = v->dev;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool changed = emb_view_render(v, cmd);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
    }
    return changed;
}

/* n frames of the view, a little time apart (timers and rAF run, posted messages are delivered). */
static void settle(emb_view *v, int n)
{
    for (int i = 0; i < n; i++) {
        (void)render_now(v);
        SDL_Delay(2);
    }
}

static uint8_t *readback(SDL_GPUDevice *dev, SDL_GPUTexture *tex, uint32_t w, uint32_t h)
{
    size_t bytes = (size_t)w * h * 4u;
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)bytes;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tbci);
    uint8_t *out = nullptr;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (tb == nullptr || cmd == nullptr) {
        if (tb != nullptr) {
            SDL_ReleaseGPUTransferBuffer(dev, tb);
        }
        return nullptr;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region;
    memset(&region, 0, sizeof region);
    region.texture = tex;
    region.w = w;
    region.h = h;
    region.d = 1;
    SDL_GPUTextureTransferInfo info;
    memset(&info, 0, sizeof info);
    info.transfer_buffer = tb;
    info.pixels_per_row = w;
    info.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &region, &info);
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
        if (mapped != nullptr) {
            out = SDL_malloc(bytes);
            SDL_memcpy(out, mapped, bytes);
            SDL_UnmapGPUTransferBuffer(dev, tb);
        }
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return out;
}

/* The view's picture (300 x 200, the rig's size) read back. */
static uint8_t *picture(emb_view *v)
{
    return readback(v->dev, emb_view_texture(v), 300, 200);
}

/* The RGB of pixel (x, y) of a 300-wide picture, as 0xRRGGBB. */
static uint32_t pixel(const uint8_t *px, int x, int y)
{
    const uint8_t *p = px + ((size_t)y * 300u + (size_t)x) * 4u;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* Opens a 300 x 200 view of html (http://127.0.0.1/page.html) and lets its frames load. */
#define OPEN_OR_SKIP(v, rt, g, html)                                                                      \
    do {                                                                                                  \
        OPEN_HTML_OR_SKIP(v, rt, g, html);                                                                \
        settle((v), 6);                                                                                   \
    } while (0)

static void click(emb_view *v, float x, float y)
{
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
}

/* ------------------------------------------------------------------ render */

/* A file: parent with <iframe src=child.html>: the child's boxes show inside the iframe's content box, clipped to it, the
 * parent's background shows where the child paints nothing, and the parent's positioned box covers the frame. */
EMB_TEST(iframe_file_child_renders_clipped_transparent_and_covered)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char dir[1024], path[1024], url[2048];
    SDL_snprintf(dir, sizeof dir, "%s/tests/pages/iframe", EMBER_SOURCE_DIR);
    EMB_REQUIRE(emb_runtime_allow_path(rt, dir));
    SDL_snprintf(path, sizeof path, "%s/parent.html", dir);
    size_t un = embi_url_from_path((embi_str){path, (uint32_t)strlen(path)}, url, sizeof url);
    EMB_REQUIRE(un < sizeof url);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g, "");
    EMB_REQUIRE(emb_view_load_url(v, url));
    settle(v, 8);
    EMB_CHECK_EQ_STR(run(v, "String(window.loads)"), "1");
    EMB_REQUIRE(embi_frames_child(v, 0) != nullptr);
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 25, 25), 0xFF0000);  /* the child's red box at (0, 0) of the frame */
    EMB_CHECK_EQ_INT((int)pixel(px, 70, 30), 0x0000FF);  /* the child paints nothing there: the parent shows */
    EMB_CHECK_EQ_INT((int)pixel(px, 115, 30), 0x00FF00); /* the child's green box, inside the frame */
    EMB_CHECK_EQ_INT((int)pixel(px, 125, 30), 0x0000FF); /* ... and clipped at the frame's right edge (x 120) */
    EMB_CHECK_EQ_INT((int)pixel(px, 70, 70), 0xFFFF00);  /* the parent's box painted above the frame covers it */
    EMB_CHECK_EQ_INT((int)pixel(px, 10, 10), 0x0000FF);  /* outside the frame */
    /* The child's text: some dark pixels in the line at the frame's (0, 30). */
    int dark = 0;
    for (int y = 50; y < 70; y++) {
        for (int x = 20; x < 60; x++) {
            dark += (pixel(px, x, y) & 0xFFu) < 0x80u;
        }
    }
    EMB_CHECK(dark > 10);
    SDL_free(px);
    rig_close(v, g, rt);
}

/* The editor's overlay: a transparent iframe with position:absolute; inset:0 over the parent: the parent shows wherever the
 * child paints nothing, and the input goes to the child. */
EMB_TEST(iframe_transparent_overlay_shows_the_parent_and_takes_input)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0;background:#f00'><div style='position:absolute;left:150px;top:0;width:150px;"
                 "height:200px;background:#0f0'></div>"
                 "<iframe id=f style='position:absolute;inset:0;width:100%;height:100%;border:0' srcdoc=\""
                 "<body style='margin:0'><div id=b style='position:absolute;left:10px;top:10px;width:30px;height:30px;"
                 "background:#00f'></div><script>window.clicks = 0; document.getElementById('b').onclick = "
                 "function () { clicks++; };</script>\"></iframe>"
                 "<script>window.pclicks = 0; document.addEventListener('click', function () { pclicks++; });</script>");
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 20, 20), 0x0000FF);   /* the child's box */
    EMB_CHECK_EQ_INT((int)pixel(px, 100, 100), 0xFF0000); /* the parent through the transparent child */
    EMB_CHECK_EQ_INT((int)pixel(px, 200, 100), 0x00FF00); /* the parent's own box, under the frame */
    SDL_free(px);
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    click(v, 20, 20);
    settle(v, 2);
    EMB_CHECK_EQ_STR(run(kid, "String(clicks)"), "1");
    EMB_CHECK_EQ_STR(run(v, "String(pclicks)"), "0");
    rig_close(v, g, rt);
}

/* position:fixed iframes that the parent moves every frame with rAF: the frame's picture follows the box. */
EMB_TEST(iframe_fixed_frame_moved_by_raf_follows)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0;background:#fff'>"
                 "<iframe id=f style='position:fixed;left:0;top:50px;width:40px;height:40px;border:0' "
                 "srcdoc=\"<body style='margin:0;background:#0f0'>\"></iframe>"
                 "<script>var x = 0; function step() { x += 10; document.getElementById('f').style.left = x + 'px';"
                 " if (x < 200) requestAnimationFrame(step); } requestAnimationFrame(step);</script>");
    settle(v, 40);
    EMB_CHECK_EQ_STR(run(v, "String(x)"), "200");
    settle(v, 2);
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 220, 70), 0x00FF00); /* the frame at its last place */
    EMB_CHECK_EQ_INT((int)pixel(px, 20, 70), 0xFFFFFF);  /* nothing left where it started */
    EMB_CHECK_EQ_INT((int)pixel(px, 120, 70), 0xFFFFFF); /* nor on the way */
    SDL_free(px);
    rig_close(v, g, rt);
}

/* ------------------------------------------------------------------ load, proxies */

EMB_TEST(iframe_load_event_and_window_proxies)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<iframe id=f srcdoc=\"<p>child</p>\"></iframe>"
                 "<script>var f = document.getElementById('f'); window.loads = 0;"
                 " f.addEventListener('load', function () { loads++; });</script>");
    EMB_CHECK_EQ_STR(run(v, "String(loads)"), "1");
    EMB_CHECK_EQ_STR(run(v, "String(f.contentWindow !== null && f.contentWindow === f.contentWindow)"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(f.contentDocument)"), "null");
    EMB_CHECK_EQ_STR(run(v, "String(window.parent === window && window.top === window)"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(window.frameElement)"), "null");
    EMB_CHECK_EQ_STR(run(v, "String(f.contentWindow.closed)"), "false");
    EMB_CHECK_EQ_STR(run(v, "String(f.contentWindow.document)"), "undefined");
    EMB_CHECK_EQ_STR(run(v, "String(f.contentWindow.window === f.contentWindow)"), "true");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK_EQ_STR(run(kid, "String(parent !== window && top === parent && parent === parent)"), "true");
    EMB_CHECK_EQ_STR(run(kid, "typeof parent.postMessage"), "function");
    EMB_CHECK_EQ_STR(run(kid, "String(parent.document)"), "undefined");
    EMB_CHECK_EQ_STR(run(kid, "String(window.frameElement)"), "null");
    EMB_CHECK_EQ_STR(run(kid, "document.body.textContent"), "child");
    rig_close(v, g, rt);
}

/* Setting src (even to the same URL) loads the frame again and fires load again; the new document has a new proxy. */
EMB_TEST(iframe_src_set_reloads)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<iframe id=f src=\"data:text/html,<p>one</p>\"></iframe>"
                 "<script>var f = document.getElementById('f'); window.loads = 0;"
                 " f.addEventListener('load', function () { loads++; });</script>");
    EMB_CHECK_EQ_STR(run(v, "String(loads)"), "1");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK_EQ_STR(run(kid, "document.body.textContent"), "one");
    (void)run(v, "window.w1 = f.contentWindow; f.src = f.src; true");
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(v, "String(loads)"), "2");
    EMB_CHECK_EQ_STR(run(v, "String(f.contentWindow !== w1 && w1.closed)"), "true");
    (void)run(v, "f.src = 'data:text/html,<p>two</p>'; true");
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(v, "String(loads)"), "3");
    kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK_EQ_STR(run(kid, "document.body.textContent"), "two");
    rig_close(v, g, rt);
}

/* ------------------------------------------------------------------ postMessage */

static const char k_msg_page[] =
    "<iframe id=f srcdoc=\"<script>"
    "window.got = []; addEventListener('message', function (e) {"
    " got.push(e.data); window.src_ok = e.source === parent; window.org = e.origin;"
    " if (e.data && e.data.a) e.source.postMessage({echo: e.data.a, list: [1, [2, 3]], m: new Map([['k', 1]])}, '*'); });"
    "</script>\"></iframe>"
    "<script>var f = document.getElementById('f'); window.got = [];"
    " addEventListener('message', function (e) { got.push(e.data); window.src_ok = e.source === f.contentWindow;"
    " window.org = e.origin; });</script>";

/* Messages both ways with nested objects, arrays and a Map; e.source is the other side's proxy; e.origin the sender's. */
EMB_TEST(iframe_post_message_both_ways)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g, k_msg_page);
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    (void)run(v, "f.contentWindow.postMessage({a: {b: [1, 2], s: 'x'}, m: new Map([['x', 2]])}, '*'); true");
    EMB_CHECK_EQ_STR(run(kid, "String(got.length)"), "0"); /* a task: not delivered synchronously */
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(kid, "String(got.length)"), "1");
    EMB_CHECK_EQ_STR(run(kid, "JSON.stringify(got[0].a)"), "{\"b\":[1,2],\"s\":\"x\"}");
    EMB_CHECK_EQ_STR(run(kid, "String(got[0].m instanceof Map && got[0].m.get('x'))"), "2");
    EMB_CHECK_EQ_STR(run(kid, "String(src_ok)"), "true");
    EMB_CHECK_EQ_STR(run(kid, "org"), "http://127.0.0.1");
    EMB_CHECK_EQ_STR(run(v, "String(got.length)"), "1");
    EMB_CHECK_EQ_STR(run(v, "JSON.stringify([got[0].echo, got[0].list])"), "[{\"b\":[1,2],\"s\":\"x\"},[1,[2,3]]]");
    EMB_CHECK_EQ_STR(run(v, "String(got[0].m instanceof Map && got[0].m.get('k'))"), "1");
    EMB_CHECK_EQ_STR(run(v, "String(src_ok)"), "true");
    EMB_CHECK_EQ_STR(run(v, "org"), "null"); /* about:srcdoc has an opaque origin */
    /* The options form, and a window's own postMessage still works. */
    (void)run(v, "f.contentWindow.postMessage('opt', {targetOrigin: '*'}); window.postMessage('self', '*'); true");
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(kid, "got[1]"), "opt");
    EMB_CHECK_EQ_STR(run(v, "got[1]"), "self");
    rig_close(v, g, rt);
}

/* A function in the message throws DataCloneError in the sender; a targetOrigin that does not match the receiver drops the
 * message silently. */
EMB_TEST(iframe_post_message_clone_error_and_target_origin)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g, k_msg_page);
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK_EQ_STR(run(v, "try { f.contentWindow.postMessage({f: function () {}}, '*'); 'none' } catch (e) { e.name }"),
                     "DataCloneError");
    EMB_CHECK_EQ_STR(run(kid, "try { parent.postMessage([function () {}], '*'); 'none' } catch (e) { e.name }"),
                     "DataCloneError");
    EMB_CHECK_EQ_STR(run(v, "try { f.contentWindow.postMessage(1, 'not a url'); 'none' } catch (e) { e.name }"),
                     "SyntaxError");
    /* The child's origin is opaque: no origin matches it, only "*". */
    (void)run(v, "f.contentWindow.postMessage('to-127', 'http://127.0.0.1'); true");
    (void)run(kid, "parent.postMessage('other', 'http://other.test'); parent.postMessage('ok', 'http://127.0.0.1');"
                   " parent.postMessage('slash', '/'); true");
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(kid, "String(got.length)"), "0");
    EMB_CHECK_EQ_STR(run(v, "got.join(',')"), "ok");
    rig_close(v, g, rt);
}

/* Transfer: an ArrayBuffer moves (detached in the sender), and a MessagePort sent to the frame keeps its peer: the two
 * realms talk over the channel both ways. */
EMB_TEST(iframe_post_message_transfers_buffers_and_ports)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<iframe id=f srcdoc=\"<script>window.got = [];"
                 " addEventListener('message', function (e) {"
                 " if (e.data instanceof ArrayBuffer) { got.push('buf:' + new Uint8Array(e.data).join('.')); return; }"
                 " var p = e.ports[0]; got.push('port:' + e.ports.length);"
                 " p.onmessage = function (m) { got.push('pm:' + m.data); p.postMessage('pong:' + m.data); }; });"
                 "</script>\"></iframe>"
                 "<script>var f = document.getElementById('f'); window.got = [];</script>");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK_EQ_STR(run(v, "var b = new Uint8Array([1, 2, 3]).buffer; f.contentWindow.postMessage(b, '*', [b]);"
                            " String(b.byteLength)"),
                     "0");
    (void)run(v, "var ch = new MessageChannel(); ch.port1.onmessage = function (m) { got.push(m.data); };"
                 " f.contentWindow.postMessage('take', {targetOrigin: '*', transfer: [ch.port2]}); true");
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(kid, "got.join(' ')"), "buf:1.2.3 port:1");
    (void)run(v, "ch.port1.postMessage('ping'); true");
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(kid, "got.join(' ')"), "buf:1.2.3 port:1 pm:ping");
    EMB_CHECK_EQ_STR(run(v, "got.join(' ')"), "pong:ping");
    /* The frame goes away: its port closes, and the parent's end has nobody to talk to (no crash, nothing delivered). */
    (void)run(v, "f.remove(); true");
    settle(v, 2);
    (void)run(v, "ch.port1.postMessage('after'); true");
    settle(v, 2);
    EMB_CHECK_EQ_STR(run(v, "got.join(' ')"), "pong:ping");
    rig_close(v, g, rt);
}

/* ------------------------------------------------------------------ lifetime, nesting */

/* Removing the iframe destroys the frame: its timers stop (no more messages), its proxy is closed. */
EMB_TEST(iframe_removed_stops_timers)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<iframe id=f srcdoc=\"<script>setInterval(function () { parent.postMessage('tick', '*'); }, 1);"
                 "</script>\"></iframe>"
                 "<script>var f = document.getElementById('f'); window.ticks = 0;"
                 " addEventListener('message', function () { ticks++; });</script>");
    settle(v, 6);
    EMB_CHECK(SDL_atoi(run(v, "String(ticks)")) > 0);
    (void)run(v, "window.w = f.contentWindow; f.remove(); true");
    settle(v, 2);
    EMB_CHECK(embi_frames_child(v, 0) == nullptr);
    int after = SDL_atoi(run(v, "String(ticks)"));
    settle(v, 8);
    EMB_CHECK_EQ_INT(SDL_atoi(run(v, "String(ticks)")), after);
    EMB_CHECK_EQ_STR(run(v, "String(w.closed)"), "true");
    EMB_CHECK_EQ_STR(run(v, "String(f.contentWindow)"), "null");
    EMB_CHECK_EQ_STR(run(v, "try { w.postMessage('x', '*'); 'sent' } catch (e) { e.name }"), "sent");
    rig_close(v, g, rt);
}

/* A frame in a frame renders, and messages pass up two levels (through the middle frame, and straight to top). */
EMB_TEST(iframe_nested_two_levels)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0;background:#fff'>"
                 "<iframe id=f style='position:absolute;left:100px;top:50px;width:120px;height:100px;border:0' srcdoc=\""
                 "<body style='margin:0'>"
                 "<iframe style='position:absolute;left:20px;top:10px;width:50px;height:40px;border:0' srcdoc='"
                 "<body style=&quot;margin:0;background:#f00&quot;><script>parent.postMessage(&quot;gc&quot;, &quot;*&quot;);"
                 " top.postMessage(&quot;to-top&quot;, &quot;*&quot;);</script>'></iframe>"
                 "<script>addEventListener('message', function (e) { parent.postMessage('relay:' + e.data, '*'); });"
                 "</script>\"></iframe>"
                 "<script>window.got = []; addEventListener('message', function (e) { got.push(e.data); });</script>");
    settle(v, 6);
    EMB_CHECK_EQ_STR(run(v, "got.slice().sort().join(',')"), "relay:gc,to-top");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    emb_view *gkid = embi_frames_child(kid, 0);
    EMB_REQUIRE(gkid != nullptr);
    EMB_CHECK_EQ_STR(run(gkid, "String(top !== parent)"), "true");
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 125, 65), 0xFF0000); /* the grandchild at (100 + 20, 50 + 10) */
    EMB_CHECK_EQ_INT((int)pixel(px, 165, 95), 0xFF0000);
    EMB_CHECK_EQ_INT((int)pixel(px, 175, 65), 0xFFFFFF); /* beside it: the transparent middle frame, the page */
    EMB_CHECK_EQ_INT((int)pixel(px, 110, 55), 0xFFFFFF);
    SDL_free(px);
    rig_close(v, g, rt);
}

/* A page that frames itself stops at depth 8: the frame below that is left empty. */
EMB_TEST(iframe_nesting_depth_is_limited)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char dir[1024], path[1024], url[2048];
    SDL_snprintf(dir, sizeof dir, "%s/tests/pages/iframe", EMBER_SOURCE_DIR);
    EMB_REQUIRE(emb_runtime_allow_path(rt, dir));
    SDL_snprintf(path, sizeof path, "%s/recurse.html", dir);
    size_t un = embi_url_from_path((embi_str){path, (uint32_t)strlen(path)}, url, sizeof url);
    EMB_REQUIRE(un < sizeof url);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g, "");
    EMB_REQUIRE(emb_view_load_url(v, url));
    settle(v, 20);
    uint32_t depth = 0;
    emb_view *k = v;
    while ((k = embi_frames_child(k, 0)) != nullptr) {
        depth++;
    }
    EMB_CHECK_EQ_INT((int)depth, (int)EMBI_FRAMES_MAX_DEPTH + 1); /* the 9th frame is the empty one */
    rig_close(v, g, rt);
}

/* ------------------------------------------------------------------ input */

static const char k_input_page[] =
    "<body style='margin:0;height:200px'>"
    "<iframe id=f style='position:absolute;left:50px;top:40px;width:150px;height:100px;border:0' srcdoc=\""
    "<body style='margin:0;height:1000px'>"
    "<button id=b style='position:absolute;left:10px;top:10px;width:60px;height:30px'>b</button>"
    "<input id=t style='position:absolute;left:10px;top:60px;width:100px;height:20px'>"
    "<script>window.log = []; var b = document.getElementById('b');"
    " b.addEventListener('click', function (e) { log.push('click:' + e.offsetX + ',' + e.offsetY); });"
    " b.addEventListener('mousedown', function (e) { log.push('down'); });"
    " document.addEventListener('mousemove', function (e) { log.push('move:' + e.clientX + ',' + e.clientY); });"
    " document.addEventListener('mouseup', function (e) { log.push('up:' + e.clientX + ',' + e.clientY); });"
    " addEventListener('focus', function () { log.push('focus'); });"
    "</script>\"></iframe>"
    "<script>var f = document.getElementById('f'); window.plog = [];"
    " ['click', 'mousedown', 'mouseup', 'mousemove'].forEach(function (t) {"
    " document.addEventListener(t, function () { plog.push(t); }); });</script>";

/* A click inside the iframe reaches the child's button with the offsets of the child's coordinates; the parent sees none. */
EMB_TEST(iframe_click_reaches_the_child_not_the_parent)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g, k_input_page);
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    click(v, 50 + 20, 40 + 15);
    settle(v, 2);
    EMB_CHECK(SDL_strstr(run(kid, "log.join(' ')"), "click:10,5") != nullptr);
    EMB_CHECK_EQ_STR(run(v, "plog.join(' ')"), "");
    /* The press focused the frame: the parent's active element is the iframe. */
    EMB_CHECK_EQ_STR(run(v, "String(document.activeElement === f)"), "true");
    /* Outside the frame the parent gets the events again. */
    click(v, 10, 10);
    EMB_CHECK(SDL_strstr(run(v, "plog.join(' ')"), "click") != nullptr);
    rig_close(v, g, rt);
}

/* A drag that starts in the child keeps going to the child when it leaves the box, up to the release. */
EMB_TEST(iframe_drag_started_in_the_child_stays_there)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g, k_input_page);
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    emb_view_mouse_move(v, 70, 55, 0);
    emb_view_mouse_button(v, 70, 55, EMB_BUTTON_PRIMARY, true, 0);
    (void)run(kid, "log.length = 0; true");
    emb_view_mouse_move(v, 120, 100, 0);
    emb_view_mouse_move(v, 10, 10, 0); /* outside the frame, in the parent */
    emb_view_mouse_button(v, 10, 10, EMB_BUTTON_PRIMARY, false, 0);
    EMB_CHECK(SDL_strncmp(run(kid, "log.join(' ')"), "move:70,60 move:-40,-30 up:-40,-30", 34) == 0);
    EMB_CHECK_EQ_STR(run(v, "plog.join(' ')"), "");
    /* After the release the pointer is the parent's again. */
    emb_view_mouse_move(v, 12, 12, 0);
    EMB_CHECK_EQ_STR(run(v, "plog.join(' ')"), "mousemove");
    rig_close(v, g, rt);
}

/* A text field in the child gets the typed text after a click in it. */
EMB_TEST(iframe_text_field_in_the_child_gets_typed_text)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g, k_input_page);
    emb_view_focus(v, true);
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    click(v, 50 + 30, 40 + 70);
    EMB_CHECK_EQ_STR(run(kid, "document.activeElement.id"), "t");
    emb_view_text_input(v, "abc", 3);
    rig_tap_key(v, "Backspace", "Backspace", 0);
    emb_view_text_input(v, "d", 1);
    EMB_CHECK_EQ_STR(run(kid, "document.getElementById('t').value"), "abd");
    EMB_CHECK_EQ_STR(run(v, "String(document.activeElement === f)"), "true");
    /* A press in the parent takes the keyboard back. */
    click(v, 10, 190);
    emb_view_text_input(v, "zz", 2);
    EMB_CHECK_EQ_STR(run(kid, "document.getElementById('t').value"), "abd");
    rig_close(v, g, rt);
}

/* The wheel over the frame scrolls the child, not the parent. */
EMB_TEST(iframe_wheel_scrolls_the_child)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0;height:2000px'>"
                 "<iframe id=f style='position:absolute;left:50px;top:40px;width:150px;height:100px;border:0' "
                 "srcdoc=\"<body style='margin:0;height:1000px'>\"></iframe>");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    emb_view_mouse_move(v, 100, 80, 0);
    emb_view_wheel(v, 100, 80, 0, 120);
    settle(v, 2);
    EMB_CHECK_EQ_INT((int)root_y(kid), 120);
    EMB_CHECK_EQ_INT((int)root_y(v), 0);
    /* Outside the frame the parent scrolls. */
    emb_view_wheel(v, 10, 10, 0, 120);
    settle(v, 2);
    EMB_CHECK_EQ_INT((int)root_y(v), 120);
    rig_close(v, g, rt);
}

/* ------------------------------------------------------------------ review fixes */

/* The file: URL of a fixture of tests/pages/iframe (the directory allowed in rt). */
static void fixture_url(emb_runtime *rt, const char *name, char *url, size_t cap)
{
    char dir[1024], path[1024];
    SDL_snprintf(dir, sizeof dir, "%s/tests/pages/iframe", EMBER_SOURCE_DIR);
    (void)emb_runtime_allow_path(rt, dir);
    SDL_snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t un = embi_url_from_path((embi_str){path, (uint32_t)strlen(path)}, url, cap);
    if (un >= cap) {
        url[0] = '\0';
    }
}

/* A page from the network: its srcdoc and data: frames are from the network too, and cannot frame a file: page. The same
 * frames in the embedder's own page can. */
EMB_TEST(iframe_inline_frames_of_a_network_page_cannot_load_local_files)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char furl[2048];
    fixture_url(rt, "child.html", furl, sizeof furl);
    EMB_REQUIRE(furl[0] != '\0');
    char html[8192];
    SDL_snprintf(html, sizeof html,
                 "<iframe id=a srcdoc=\"<iframe src='%s'></iframe>\"></iframe>"
                 "<iframe id=b src=\"data:text/html,<iframe src='%s'></iframe>\"></iframe>",
                 furl, furl);
    for (int net = 1; net >= 0; net--) {
        embi_gpu *g = nullptr;
        emb_view *v = nullptr;
        OPEN_HTML_OR_SKIP(v, rt, g, "");
        v->net_doc_next = net != 0; /* as netload marks a page that came from the network */
        EMB_REQUIRE(emb_view_load_html(v, html, strlen(html), "http://127.0.0.1/page.html"));
        settle(v, 8);
        for (uint32_t i = 0; i < 2; i++) {
            emb_view *kid = embi_frames_child(v, i);
            EMB_REQUIRE(kid != nullptr);
            EMB_CHECK(kid->net_doc == (net != 0));
            emb_view *gkid = embi_frames_child(kid, 0);
            EMB_REQUIRE(gkid != nullptr);
#ifdef EMBER_NET
            const char *want = net ? "" : "child"; /* refused: the frame is left empty */
#else
            const char *want = "child"; /* no network stack: no page is from the network, nothing is refused */
#endif
            EMB_CHECK_EQ_STR(run(gkid, "document.body ? document.body.textContent.trim() : ''"), want);
            EMB_CHECK(gkid->net_doc == (net != 0));
        }
        emb_view_destroy(v);
        embi_gpu_destroy(g);
    }
    emb_runtime_destroy(rt);
}

/* The root's scale and colour scheme reach its frames, even when the layout does not change. */
EMB_TEST(iframe_follows_the_root_scale_and_color_scheme)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0'><iframe id=f style='width:100px;height:50px;border:0' srcdoc=\"<style>"
                 "body { margin: 0; background: #fff } @media (prefers-color-scheme: dark) { body { background: #000 } }"
                 "</style>\"></iframe>");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK(kid->scale == 1.0f && kid->dev_w == 100u && kid->dev_h == 50u);
    emb_view_set_scale(v, 2.0f);
    settle(v, 2);
    EMB_CHECK(kid->scale == 2.0f);
    EMB_CHECK_EQ_INT((int)kid->dev_w, 200);
    EMB_CHECK_EQ_INT((int)kid->dev_h, 100);
    EMB_CHECK_EQ_STR(run(kid, "String(devicePixelRatio)"), "2");
    emb_view_set_color_scheme(v, EMB_SCHEME_DARK);
    settle(v, 3);
    EMB_CHECK(kid->scheme == EMB_SCHEME_DARK);
    emb_view_set_scale(v, 1.0f);
    settle(v, 2);
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 50, 25), 0x000000); /* the frame's dark background */
    SDL_free(px);
    rig_close(v, g, rt);
}

/* A text field focused in a frame asks its root's window for text input, with the caret in the root's coordinates. */
EMB_TEST(iframe_text_input_area_goes_to_the_root_window)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g, k_input_page);
    emb_view_focus(v, true);
    click(v, 50 + 30, 40 + 70);
    settle(v, 1);
    embi_text_area_call t = embi_input_last_text_area();
    EMB_CHECK(t.window == v);
    EMB_CHECK(t.on);
    EMB_CHECK(t.rect[0] >= 60.0f && t.rect[0] < 160.0f); /* the field is at (10, 60) of the frame at (50, 40) */
    EMB_CHECK(t.rect[1] >= 100.0f && t.rect[1] < 125.0f);
    click(v, 10, 190); /* the parent takes the keyboard back: text input stops */
    t = embi_input_last_text_area();
    EMB_CHECK(t.window == v && !t.on);
    rig_close(v, g, rt);
}

/* The pointer mapping: a clipped-away part of an iframe is the parent's; a rotated iframe maps through its transform; an
 * iframe in a scrolled container and a fixed iframe in a scrolled page map with the scroll offsets. */
EMB_TEST(iframe_pointer_mapping_clips_transforms_and_scrolling)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
#define K_FRAME_DOC                                                                                                    \
    "srcdoc=\"<body style='margin:0;height:300px'><script>window.log = []; document.addEventListener('mousedown',"   \
    " function (e) { log.push(e.clientX + ',' + e.clientY); });</script>\""
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0;height:2000px'>"
                 "<div style='position:absolute;left:0;top:0;width:60px;height:40px;overflow:hidden'>"
                 "<iframe id=a style='width:100px;height:40px;border:0' " K_FRAME_DOC "></iframe></div>"
                 "<iframe id=b style='position:absolute;left:100px;top:0;width:80px;height:40px;border:0;"
                 "transform:rotate(180deg)' " K_FRAME_DOC "></iframe>"
                 "<div id=sc style='position:absolute;left:0;top:60px;width:100px;height:60px;overflow:auto'>"
                 "<div style='height:100px'></div>"
                 "<iframe id=c style='display:block;width:80px;height:40px;border:0' " K_FRAME_DOC "></iframe>"
                 "<div style='height:100px'></div></div>"
                 "<iframe id=d style='position:fixed;left:200px;top:100px;width:80px;height:40px;border:0' " K_FRAME_DOC
                 "></iframe>"
                 "<script>window.plog = []; document.addEventListener('mousedown', function (e) {"
                 " plog.push(e.clientX + ',' + e.clientY); });</script>");
#undef K_FRAME_DOC
    emb_view *a = embi_frames_child(v, 0), *b = embi_frames_child(v, 1), *c = embi_frames_child(v, 2),
             *d = embi_frames_child(v, 3);
    EMB_REQUIRE(a != nullptr && b != nullptr && c != nullptr && d != nullptr);
    /* a: visible up to x 60; the part beyond is clipped away, so a press there is the parent's. */
    click(v, 30, 10);
    click(v, 80, 10);
    EMB_CHECK_EQ_STR(run(a, "log.join(' ')"), "30,10");
    EMB_CHECK_EQ_STR(run(v, "plog.join(' ')"), "80,10");
    /* b: turned half a turn about its centre: its top left corner on screen is its bottom right. */
    click(v, 105, 5);
    EMB_CHECK_EQ_STR(run(b, "log.join(' ')"), "75,35");
    /* c: 100px down in a scroller scrolled by 70: on screen at y 60 + 100 - 70 = 90. */
    (void)run(v, "document.getElementById('sc').scrollTop = 70; true");
    settle(v, 2);
    click(v, 10, 95);
    EMB_CHECK_EQ_STR(run(c, "log.join(' ')"), "10,5");
    /* d: position:fixed in a scrolled page: the root's scroll does not move it, so it is painted, reported and hit at its
     * place in the viewport (top 100), and the press there goes to it. */
    (void)run(v, "window.scrollTo(0, 50); true");
    settle(v, 2);
    EMB_CHECK_EQ_STR(run(v, "String(window.scrollY)"), "50");
    EMB_CHECK_EQ_STR(run(v, "String(document.getElementById('d').getBoundingClientRect().top)"), "100");
    (void)run(d, "document.body.style.background = '#0f0'; true");
    settle(v, 2);
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 210, 110), 0x00FF00);
    EMB_CHECK(pixel(px, 210, 60) != 0x00FF00); /* not where a scrolled box would be */
    SDL_free(px);
    click(v, 210, 110);
    EMB_CHECK_EQ_STR(run(d, "log.join(' ')"), "10,10");
    EMB_CHECK_EQ_STR(run(v, "plog.join(' ')"), "80,10");
    rig_close(v, g, rt);
}

/* contentWindow is there as soon as the iframe is inserted (its initial empty document), and the frame's first document
 * keeps it. */
EMB_TEST(iframe_content_window_on_insertion)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g, "<body></body>");
    EMB_CHECK_EQ_STR(run(v, "var f = document.createElement('iframe'); String(f.contentWindow)"), "null");
    EMB_CHECK_EQ_STR(run(v, "f.srcdoc = '<p>late</p>'; document.body.append(f); window.w = f.contentWindow;"
                            " try { w.postMessage('early', '*'); String(w !== null && !w.closed) } catch (e) { e.name }"),
                     "true");
    settle(v, 4);
    EMB_CHECK_EQ_STR(run(v, "String(f.contentWindow === w && !w.closed)"), "true");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK_EQ_STR(run(kid, "document.body.textContent"), "late");
    (void)run(kid, "addEventListener('message', function (e) { window.m = e.data; }); true");
    (void)run(v, "w.postMessage('now', '*'); true");
    settle(v, 2);
    EMB_CHECK_EQ_STR(run(kid, "String(window.m)"), "now");
    rig_close(v, g, rt);
}

/* A parent script that moves the focus off the iframe takes the keyboard from the frame at once (its field blurs); focus
 * handlers that move the focus while emb_view_focus walks the chain are safe. */
EMB_TEST(iframe_focus_moved_by_the_parent_blurs_the_frame)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0;height:200px'><input id=p style='position:absolute;left:0;top:180px'>"
                 "<iframe id=f style='position:absolute;left:50px;top:40px;width:150px;height:100px;border:0' srcdoc=\""
                 "<input id=t style='position:absolute;left:10px;top:10px;width:100px;height:20px'><script>"
                 "window.blurs = 0; document.getElementById('t').addEventListener('blur', function () { blurs++; });"
                 "</script>\"></iframe>"
                 "<script>var f = document.getElementById('f');</script>");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    emb_view_focus(v, true);
    click(v, 50 + 20, 40 + 15);
    EMB_CHECK_EQ_STR(run(kid, "document.activeElement.id"), "t");
    (void)run(v, "document.getElementById('p').focus(); true");
    settle(v, 1);
    EMB_CHECK_EQ_STR(run(kid, "String(blurs)"), "1");
    emb_view_text_input(v, "xy", 2);
    EMB_CHECK_EQ_STR(run(v, "document.getElementById('p').value"), "xy");
    EMB_CHECK_EQ_STR(run(kid, "document.getElementById('t').value"), "");
    /* The window's focus going and coming back while a handler moves the focus out of the frame. */
    click(v, 50 + 20, 40 + 15);
    (void)run(kid, "addEventListener('focus', function () { window.fo = 1; }); true");
    (void)run(v, "addEventListener('focus', function () { document.getElementById('p').focus(); }); true");
    emb_view_focus(v, false);
    emb_view_focus(v, true);
    emb_view_focus(v, false);
    emb_view_focus(v, true);
    (void)run(v, "document.getElementById('p').focus(); true");
    emb_view_focus(v, false); /* the chain is read again at each step: the frame is no longer in it */
    emb_view_focus(v, true);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "p");
    rig_close(v, g, rt);
}

typedef struct pic_site {
    int fetches;
} pic_site;

/* A 4 x 4 opaque PNG of #00C853. */
static const uint8_t k_pic_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x02, 0x00, 0x00, 0x00, 0x26, 0x93, 0x09, 0x29, 0x00, 0x00, 0x00, 0x0F, 0x49,
    0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0x38, 0x11, 0x8C, 0x40, 0xC4, 0x71, 0x00, 0xC6, 0xF3, 0x11, 0xB1, 0xA3,
    0x6A, 0x73, 0x24, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

static bool pic_handler(void *user, const char *url, size_t url_len, emb_resource *out)
{
    (void)url;
    (void)url_len;
    ((pic_site *)user)->fetches++;
    out->data = k_pic_png;
    out->len = sizeof k_pic_png;
    return true;
}

/* A frame that loads again fetches its images again (the shared cache forgets the old document's images). */
EMB_TEST(iframe_reload_fetches_its_images_again)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    pic_site site = {0};
    EMB_REQUIRE(emb_runtime_set_resource_handler(rt, "mem", pic_handler, &site));
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<iframe id=f style='width:100px;height:50px;border:0' srcdoc=\"<body style='margin:0'>"
                 "<img src='mem://site/pic.png' style='width:20px;height:20px;display:block'>\"></iframe>");
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 10, 10), 0x00C853);
    SDL_free(px);
    int first = site.fetches;
    EMB_CHECK(first >= 1);
    (void)run(v, "var f = document.getElementById('f'); f.srcdoc = f.srcdoc; true");
    settle(v, 4);
    EMB_CHECK(site.fetches > first);
    px = picture(v);
    EMB_REQUIRE(px != nullptr);
    EMB_CHECK_EQ_INT((int)pixel(px, 10, 10), 0x00C853);
    SDL_free(px);
    rig_close(v, g, rt);
}

/* A frame at a fractional place and size is drawn 1:1: a one-pixel line in it stays one sharp pixel. */
EMB_TEST(iframe_fractional_box_draws_sharp)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_OR_SKIP(v, rt, g,
                 "<body style='margin:0;background:#fff'>"
                 "<iframe style='position:absolute;left:20.4px;top:10.3px;width:100.4px;height:50.3px;border:0' srcdoc=\""
                 "<body style='margin:0;background:#fff'><div style='position:absolute;left:10px;top:0;width:1px;"
                 "height:50px;background:#000'></div>\"></iframe>");
    uint8_t *px = picture(v);
    EMB_REQUIRE(px != nullptr);
    int black = 0, gray = 0;
    for (int x = 20; x < 120; x++) {
        uint32_t c = pixel(px, x, 30);
        black += c == 0x000000;
        gray += c != 0x000000 && c != 0xFFFFFF;
    }
    EMB_CHECK_EQ_INT(black, 1);
    EMB_CHECK_EQ_INT(gray, 0);
    SDL_free(px);
    rig_close(v, g, rt);
}

/* The embedder's pattern: a file: page with a file: frame; the frame says it is ready with parent.postMessage({..}, '*'),
 * the page knows it by e.source === frame.contentWindow and answers with frame.contentWindow.postMessage(.., '*'). */
EMB_TEST(iframe_file_host_and_frame_talk)
{
    emb_runtime *rt = emb_runtime_create(nullptr);
    EMB_REQUIRE(rt != nullptr);
    char url[2048];
    fixture_url(rt, "host.html", url, sizeof url);
    EMB_REQUIRE(url[0] != '\0');
    embi_gpu *g = nullptr;
    emb_view *v = nullptr;
    OPEN_HTML_OR_SKIP(v, rt, g, "");
    EMB_REQUIRE(emb_view_load_url(v, url));
    settle(v, 8);
    EMB_CHECK_EQ_STR(run(v, "JSON.stringify(got)"),
                     "[{\"data\":{\"type\":\"ready\",\"panel\":{\"name\":\"Inspector\",\"size\":[320,240]}},"
                     "\"fromFrame\":true,\"origin\":\"null\"}]");
    emb_view *kid = embi_frames_child(v, 0);
    EMB_REQUIRE(kid != nullptr);
    EMB_CHECK_EQ_STR(run(kid, "JSON.stringify(got)"),
                     "[{\"data\":{\"type\":\"hello\",\"items\":[1,2,3]},\"fromParent\":true}]");
    rig_close(v, g, rt);
}

RIG_MAIN()
