/*
 * bench_editor: the cost of the messages a game editor's host sends to its page. The page is the Falcon engine's editor (a
 * CSS grid and flex app shell of about 1,400 elements: a virtualized hierarchy list, an inspector, an asset grid), loaded
 * from file: into an offscreen view of 1600x900 CSS px at scale 1 in the dark scheme. The host's messages (one per line of
 * a JSONL file, each line the message text) are posted through the view's host channel ("falcon": the page's
 * window.falcon.onMessage) one at a time; after each, the view renders until it is idle (the page handles a message in a
 * setTimeout(0) task and re-renders its panels, reading layout in between: those reads force synchronous layouts).
 *
 *   bench_editor [--bundle DIR] [--messages FILE] [--rounds N] [--in-flight N] [--gpu-load N] [--system-fonts] [--quiet]
 *                [--verbose]
 *
 * --bundle DIR     the editor bundle (index.html and its scripts and styles; default ../falcon-editor-bundle)
 * --messages FILE  the messages (default ../editor_edit_messages.jsonl)
 * --rounds N       replay the messages N times (default 1; the rounds after the first are the steady state)
 * --in-flight N    frames in flight, as a host with a swapchain has them (default 0: each frame is waited for, so the GPU
 *                  is idle when the next one records; 2 is what Falcon has)
 * --gpu-load N     the GPU work of a game under each frame: N copies of a 64 MB texture, submitted before it (default 0)
 * --system-fonts   the runtime indexes and uses the machine's fonts (default off, so the numbers do not depend on them)
 * --quiet          only the summary lines
 * --verbose        the view's stats of each message's last frame
 *
 * Per message: the wall time from the post to the idle view (all frames: the script, its forced layouts, the frame's own
 * style, layout and paint, the GPU recording; the wait for the GPU is not counted), the number of embi_layout_document calls
 * and their time (the frame's own layout is one of them; the rest are layouts the script forced), what each did (NONE,
 * PAINT, SURGERY over the kept tree, PART: a partial relayout, FULL) and the boxes they laid out, and the recording time of
 * its frames (emb_view_stats.gpu_ms: passes, draws and uploads into the command buffer): their sum and the largest. Then
 * totals by message type. EMBER_VERIFY_INCREMENTAL=1 checks every incremental layout against a full one (slow; the times then mean nothing).
 * Use a Release build for numbers.
 */
#include <ember/ember.h>
#include <ember/ember_script.h>

#include "base/base.h"
#include "embed/doc.h"
#include "embed/embed_int.h"
#include "embed/script_int.h"
#include "layout/layout.h"
#include "style/style.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define MAX_MSGS 4096
#define MAX_TYPES 64
#define IDLE_FRAMES 3
#define MAX_FRAMES_PER_MSG 240

static void quiet_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    if (level >= EMB_LOG_WARN) {
        fprintf(stderr, "bench_editor: %s\n", message);
    }
}

static uint32_t g_replies;
static void on_host_message(void *user, const char *message, size_t len)
{
    (void)user;
    (void)message;
    (void)len;
    g_replies++;
}

static double ticks_ms(uint64_t t)
{
    return (double)t * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

typedef struct msg {
    const char *text;
    size_t len;
    char type[48];
} msg;

/* The "type" of a message: the value of its first "type" key. */
static void msg_type(msg *m)
{
    SDL_strlcpy(m->type, "?", sizeof m->type);
    const char *k = SDL_strstr(m->text, "\"type\"");
    if (k == nullptr) {
        return;
    }
    const char *q = SDL_strchr(k + 6, '"');
    if (q == nullptr) {
        return;
    }
    const char *e = SDL_strchr(q + 1, '"');
    if (e == nullptr) {
        return;
    }
    size_t n = (size_t)(e - q - 1);
    if (n >= sizeof m->type) {
        n = sizeof m->type - 1;
    }
    memcpy(m->type, q + 1, n);
    m->type[n] = '\0';
}

/* How frames reach the GPU. in_flight 0: each frame is waited for before the next (the GPU is idle when a frame records).
 * in_flight N: like a host with a swapchain, up to N frames are in flight, and the oldest is waited for only when a new
 * one would make N + 1, so a frame records while the GPU still runs the ones before it. gpu_load N: each frame first
 * submits N copies of a 64 MB texture (a game's own GPU work, in its own command buffer), so the GPU is busy. */
#define MAX_IN_FLIGHT 8
static int g_in_flight;
static int g_gpu_load;
static SDL_GPUFence *g_fences[MAX_IN_FLIGHT + 1];
static int g_nfences;
static SDL_GPUTexture *g_load_src, *g_load_dst;

/* The recording times of a message's frames (emb_view_stats.gpu_ms): their sum and the largest. */
static double g_gpu_sum, g_gpu_max;

static void wait_oldest(SDL_GPUDevice *dev)
{
    SDL_WaitForGPUFences(dev, true, &g_fences[0], 1);
    SDL_ReleaseGPUFence(dev, g_fences[0]);
    for (int i = 1; i < g_nfences; i++) {
        g_fences[i - 1] = g_fences[i];
    }
    g_nfences--;
}

static void gpu_load(SDL_GPUDevice *dev)
{
    if (g_gpu_load <= 0) {
        return;
    }
    if (g_load_src == nullptr) {
        const SDL_GPUTextureCreateInfo ci = {
            .type = SDL_GPU_TEXTURETYPE_2D,
            .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            .width = 4096,
            .height = 4096,
            .layer_count_or_depth = 1,
            .num_levels = 1,
        };
        g_load_src = SDL_CreateGPUTexture(dev, &ci);
        g_load_dst = SDL_CreateGPUTexture(dev, &ci);
        if (g_load_src == nullptr || g_load_dst == nullptr) {
            g_gpu_load = 0;
            return;
        }
    }
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    for (int i = 0; i < g_gpu_load; i++) {
        const SDL_GPUTextureLocation a = {.texture = (i & 1) ? g_load_dst : g_load_src};
        const SDL_GPUTextureLocation b = {.texture = (i & 1) ? g_load_src : g_load_dst};
        SDL_CopyGPUTextureToTexture(cp, &a, &b, 4096, 4096, 1, false);
    }
    SDL_EndGPUCopyPass(cp);
    (void)SDL_SubmitGPUCommandBuffer(cmd);
}

/* One frame: render and submit (the waits for the GPU are not counted). Returns the render's answer; *ticks gets the host
 * time. */
static bool frame(SDL_GPUDevice *dev, emb_view *v, uint64_t *ticks)
{
    gpu_load(dev);
    uint64_t t0 = SDL_GetPerformanceCounter();
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd == nullptr) {
        return false;
    }
    bool changed = emb_view_render(v, cmd);
    SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    *ticks += SDL_GetPerformanceCounter() - t0;
    emb_view_stats vs;
    memset(&vs, 0, sizeof vs);
    vs.size = sizeof vs;
    emb_view_stats_get(v, &vs);
    g_gpu_sum += vs.gpu_ms;
    g_gpu_max = vs.gpu_ms > g_gpu_max ? vs.gpu_ms : g_gpu_max;
    if (f != nullptr) {
        g_fences[g_nfences++] = f;
        while (g_nfences > g_in_flight) {
            wait_oldest(dev);
        }
    }
    return changed;
}

/* Renders until IDLE_FRAMES frames in a row change nothing (and no layout ran in them). Returns the frames rendered. */
static uint32_t run_until_idle(SDL_GPUDevice *dev, emb_view *v, embi_layout_engine *le, uint64_t *ticks)
{
    uint32_t n = 0, quiet = 0;
    while (n < MAX_FRAMES_PER_MSG && quiet < IDLE_FRAMES) {
        embi_layout_prof p0;
        embi_layout_prof_get(le, &p0);
        bool changed = frame(dev, v, ticks);
        embi_layout_prof p1;
        embi_layout_prof_get(le, &p1);
        bool laid = p1.kinds[EMBI_LU_SURGERY] + p1.kinds[EMBI_LU_FULL] + p1.kinds[EMBI_LAYOUT_PROF_PARTIAL] !=
                    p0.kinds[EMBI_LU_SURGERY] + p0.kinds[EMBI_LU_FULL] + p0.kinds[EMBI_LAYOUT_PROF_PARTIAL];
        quiet = changed || laid ? 0 : quiet + 1;
        n++;
    }
    return n;
}

typedef struct tally {
    char type[48];
    uint32_t n;
    double wall, lay, plan, style, max_wall, gpu, max_gpu;
    uint64_t calls, kinds[EMBI_LAYOUT_PROF_KINDS], boxes, reused;
} tally;

static tally *tally_for(tally *t, uint32_t *nt, const char *type)
{
    for (uint32_t i = 0; i < *nt; i++) {
        if (strcmp(t[i].type, type) == 0) {
            return &t[i];
        }
    }
    if (*nt == MAX_TYPES) {
        return &t[MAX_TYPES - 1];
    }
    tally *x = &t[(*nt)++];
    memset(x, 0, sizeof *x);
    SDL_strlcpy(x->type, type, sizeof x->type);
    return x;
}

int main(int argc, char **argv)
{
    const char *bundle = "../falcon-editor-bundle", *msgs_path = "../editor_edit_messages.jsonl";
    int rounds = 1;
    bool system_fonts = false, quiet = false, verbose = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) {
            bundle = argv[++i];
        } else if (strcmp(argv[i], "--messages") == 0 && i + 1 < argc) {
            msgs_path = argv[++i];
        } else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
            rounds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--system-fonts") == 0) {
            system_fonts = true;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--in-flight") == 0 && i + 1 < argc) {
            const int n = atoi(argv[++i]);
            g_in_flight = SDL_clamp(n, 0, MAX_IN_FLIGHT);
        } else if (strcmp(argv[i], "--gpu-load") == 0 && i + 1 < argc) {
            const int n = atoi(argv[++i]);
            g_gpu_load = SDL_max(n, 0);
        } else {
            fprintf(stderr, "usage: %s [--bundle DIR] [--messages FILE] [--rounds N] [--in-flight N] [--gpu-load N] [--system-fonts] "
                            "[--quiet] [--verbose]\n", argv[0]);
            return 0;
        }
    }
    if (rounds < 1) {
        rounds = 1;
    }
    size_t mlen = 0;
    char *mdata = SDL_LoadFile(msgs_path, &mlen);
    if (mdata == nullptr) {
        printf("bench_editor: cannot read %s: %s; nothing measured\n", msgs_path, SDL_GetError());
        return 0;
    }
    static msg msgs[MAX_MSGS];
    uint32_t nm = 0;
    for (char *p = mdata; p < mdata + mlen && nm < MAX_MSGS;) {
        char *e = memchr(p, '\n', (size_t)(mdata + mlen - p));
        char *end = e != nullptr ? e : mdata + mlen;
        char *t = end;
        while (t > p && (t[-1] == '\r' || t[-1] == ' ')) {
            t--;
        }
        if (t > p) {
            *t = '\0';
            msgs[nm].text = p;
            msgs[nm].len = (size_t)(t - p);
            msg_type(&msgs[nm]);
            nm++;
        }
        p = end + 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("bench_editor: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (dev == nullptr) {
        printf("bench_editor: no GPU device (%s); nothing measured\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }
    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = quiet_log;
    rd.flags = system_fonts ? 0u : EMB_RUNTIME_NO_SYSTEM_FONTS;
    emb_runtime *rt = emb_runtime_create(&rd);
    char *abs_bundle = SDL_malloc(4096);
    {
        /* emb_runtime_allow_path wants the directory as the loader sees the page's URL */
        char *cwd = SDL_GetCurrentDirectory();
        bool rel = !(bundle[0] == '/' || bundle[0] == '\\' || (bundle[0] != '\0' && bundle[1] == ':'));
        SDL_snprintf(abs_bundle, 4096, "%s%s", rel && cwd != nullptr ? cwd : "", bundle);
        SDL_free(cwd);
        for (char *c = abs_bundle; *c != '\0'; c++) {
            if (*c == '\\') {
                *c = '/';
            }
        }
    }
    if (rt == nullptr || !emb_runtime_allow_path(rt, abs_bundle)) {
        printf("bench_editor: the runtime could not be created or %s not allowed\n", abs_bundle);
        return 0;
    }
    emb_view_desc vd;
    memset(&vd, 0, sizeof vd);
    vd.size = sizeof vd;
    vd.mode = EMB_VIEW_OFFSCREEN;
    vd.width = 1600;
    vd.height = 900;
    vd.scale = 1.0f;
    vd.device = dev;
    vd.scheme = EMB_SCHEME_DARK;
    emb_view *v = emb_view_create(rt, &vd);
    if (v == nullptr || !emb_view_set_host_channel(v, "falcon", on_host_message, nullptr)) {
        printf("bench_editor: the view could not be created\n");
        return 0;
    }
    char url[4200];
    SDL_snprintf(url, sizeof url, "%s/index.html", abs_bundle);
    uint64_t load_ticks = 0;
    uint64_t lt0 = SDL_GetPerformanceCounter();
    if (!emb_view_load_url(v, url)) {
        printf("bench_editor: %s did not load\n", url);
        return 0;
    }
    load_ticks = SDL_GetPerformanceCounter() - lt0;
    embi_layout_engine *le = embi_doc_layout_engine(v->doc);
    uint64_t first_ticks = 0;
    uint32_t first_frames = run_until_idle(dev, v, le, &first_ticks);
    printf("bench_editor: GPU driver %s, %s, %u messages x %d round(s), %d frame(s) in flight, GPU load %d\n",
           SDL_GetGPUDeviceDriver(dev), url, nm, rounds, g_in_flight, g_gpu_load);
    printf("load %.1f ms, first frames until idle: %u in %.1f ms\n", ticks_ms(load_ticks), first_frames, ticks_ms(first_ticks));
    {
        uint32_t nboxes = 0, nfrags = 0;
        size_t bytes = 0;
        embi_layout_stats(le, embi_doc_dom(v->doc), &nboxes, &nfrags, &bytes);
        printf("boxes %u, fragments %u, layout arenas %zu KB\n", nboxes, nfrags, bytes / 1024u);
    }
    if (!quiet) {
        printf("%4s %-20s %6s %8s %7s %7s %4s %8s %7s %7s %-28s %7s %7s\n", "#", "type", "bytes", "wall_ms", "gpu_ms",
               "gpu_max", "lays", "lay_ms", "plan_ms", "sty_ms", "kinds none/paint/surg/part/full", "boxes", "reused");
    }

    static tally tl[MAX_TYPES];
    uint32_t nt = 0;
    tally all;
    memset(&all, 0, sizeof all);
    SDL_strlcpy(all.type, "all", sizeof all.type);
    for (int r = 0; r < rounds; r++) {
        for (uint32_t i = 0; i < nm; i++) {
            embi_layout_prof p0, p1;
            embi_layout_prof_get(le, &p0);
            uint64_t ticks = 0;
            g_gpu_sum = 0.0;
            g_gpu_max = 0.0;
            emb_view_post_host_message(v, msgs[i].text, msgs[i].len);
            uint32_t frames = run_until_idle(dev, v, le, &ticks);
            embi_layout_prof_get(le, &p1);
            double wall = ticks_ms(ticks), lay = ticks_ms(p1.ticks - p0.ticks);
            double plan = ticks_ms(p1.plan_ticks - p0.plan_ticks), sty = ticks_ms(p1.style_ticks - p0.style_ticks);
            uint64_t k[EMBI_LAYOUT_PROF_KINDS];
            for (int j = 0; j < EMBI_LAYOUT_PROF_KINDS; j++) {
                k[j] = p1.kinds[j] - p0.kinds[j];
            }
            uint64_t boxes = p1.boxes - p0.boxes, reused = p1.reused - p0.reused, calls = p1.calls - p0.calls;
            if (verbose) {
                emb_view_stats vs;
                memset(&vs, 0, sizeof vs);
                vs.size = sizeof vs;
                emb_view_stats_get(v, &vs);
                uint32_t nb = 0, nf = 0;
                size_t by = 0;
                embi_layout_stats(le, embi_doc_dom(v->doc), &nb, &nf, &by);
                printf("     last frame: style %.2f layout %.2f display list %.2f gpu %.2f ms, %u items; %u boxes %u fragments\n",
                       vs.style_ms, vs.layout_ms, vs.display_list_ms, vs.gpu_ms, vs.items, nb, nf);
            }
            if (!quiet) {
                char kinds[64];
                SDL_snprintf(kinds, sizeof kinds, "%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64, k[EMBI_LU_NONE],
                             k[EMBI_LU_PAINT], k[EMBI_LU_SURGERY], k[EMBI_LAYOUT_PROF_PARTIAL], k[EMBI_LU_FULL]);
                printf("%4u %-20s %6zu %8.2f %7.2f %7.2f %4" PRIu64 " %8.2f %7.2f %7.2f %-28s %7" PRIu64 " %7" PRIu64 "%s\n",
                       i + 1, msgs[i].type, msgs[i].len, wall, g_gpu_sum, g_gpu_max, calls, lay, plan, sty, kinds, boxes,
                       reused,
                       frames >= MAX_FRAMES_PER_MSG ? "  (never idle)" : "");
            }
            tally *ts[2] = {tally_for(tl, &nt, msgs[i].type), &all};
            for (int q = 0; q < 2; q++) {
                tally *t = ts[q];
                t->n++;
                t->wall += wall;
                t->lay += lay;
                t->plan += plan;
                t->style += sty;
                t->max_wall = wall > t->max_wall ? wall : t->max_wall;
                t->gpu += g_gpu_sum;
                t->max_gpu = g_gpu_max > t->max_gpu ? g_gpu_max : t->max_gpu;
                t->calls += calls;
                t->boxes += boxes;
                t->reused += reused;
                for (int j = 0; j < EMBI_LAYOUT_PROF_KINDS; j++) {
                    t->kinds[j] += k[j];
                }
            }
        }
    }
    printf("\nby type (averages per message; kinds and boxes are totals)\n");
    printf("%-20s %4s %8s %8s %7s %7s %6s %8s %7s %7s %-28s %8s %8s\n", "type", "n", "wall_ms", "max_ms", "gpu_ms",
           "gpu_max", "lays", "lay_ms", "plan_ms", "sty_ms", "kinds none/paint/surg/part/full", "boxes", "reused");
    for (uint32_t i = 0; i <= nt; i++) {
        const tally *t = i < nt ? &tl[i] : &all;
        double n = t->n > 0 ? (double)t->n : 1.0;
        char kinds[80];
        SDL_snprintf(kinds, sizeof kinds, "%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64, t->kinds[EMBI_LU_NONE],
                     t->kinds[EMBI_LU_PAINT], t->kinds[EMBI_LU_SURGERY], t->kinds[EMBI_LAYOUT_PROF_PARTIAL],
                     t->kinds[EMBI_LU_FULL]);
        printf("%-20s %4u %8.2f %8.2f %7.2f %7.2f %6.1f %8.2f %7.2f %7.2f %-28s %8" PRIu64 " %8" PRIu64 "\n", t->type,
               t->n, t->wall / n, t->max_wall, t->gpu / n, t->max_gpu, (double)t->calls / n, t->lay / n, t->plan / n,
               t->style / n, kinds, t->boxes, t->reused);
    }
    {
        uint32_t nboxes = 0, nfrags = 0;
        size_t bytes = 0;
        embi_layout_stats(le, embi_doc_dom(v->doc), &nboxes, &nfrags, &bytes);
        printf("at the end: boxes %u, fragments %u, layout arenas %zu KB\n", nboxes, nfrags, bytes / 1024u);
    }
    printf("replies from the page: %u\n", g_replies);
    while (g_nfences > 0) {
        wait_oldest(dev);
    }
    if (g_load_src != nullptr) {
        SDL_ReleaseGPUTexture(dev, g_load_src);
    }
    if (g_load_dst != nullptr) {
        SDL_ReleaseGPUTexture(dev, g_load_dst);
    }
    emb_view_destroy(v);
    emb_runtime_destroy(rt);
    SDL_free(abs_bundle);
    SDL_free(mdata);
    SDL_DestroyGPUDevice(dev);
    SDL_Quit();
    return 0;
}
