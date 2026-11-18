/* Two Ember views as panels in a 3D scene, drawn by the host.
 *
 *   world_panel [--frames N]
 *
 * The host owns the window, the SDL_GPUDevice and the scene. It makes one atlas texture (2048 x
 * 1024, RGBA8, with a full mip chain) and gives each view a rectangle of it: a menu page on the
 * left and a terminal page on the right. Each frame the views are drawn into their rectangles with
 * emb_view_render_to, the atlas mips are regenerated when a view redrew, and the atlas is sampled
 * on two quads that float in a perspective view. The camera moves in and out over time, so each
 * panel's size on screen changes, and each panel's raster scale follows that size: text stays sharp
 * up close and costs less far away.
 *
 * Keys and mouse: the mouse ray is cast through the inverse view-projection and hits a panel's
 * plane; the hit point becomes CSS px for the view under it. The wheel scrolls the panel under the
 * pointer, or dollies the camera over empty space. W and S dolly the camera. Esc quits.
 *
 * The scene's quads are drawn with shaders/host_panel.slang (ember_shaders/host_panel.h). Desktop
 * only: it uses a plain main().
 */

#define SDL_MAIN_HANDLED
#include <ember/ember.h>

#include "ember_shaders/host_panel.h"
#include "gpu/gpu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#define WIN_W 1100
#define WIN_H 620
#define ATLAS_W 2048
#define ATLAS_H 1024
#define ATLAS_LEVELS 12 /* floor(log2(2048)) + 1 */
#define GUTTER                                                                                                         \
    32 /* device px between the two slots: mips of the atlas bleed across slots at far distances                       \
        */
#define SLOT_W ((ATLAS_W - GUTTER) / 2)
#define NPANELS 2
#define WHEEL_PX 53.0f
#define DOLLY_KEY_SPEED 1.5f  /* world units per second */
#define DOLLY_WHEEL 0.3f      /* world units per wheel notch over empty space */
#define CAMERA_FOV 0.7853982f /* 45 degrees, vertical */

static const char k_menu_page[] =
    "<!doctype html><meta charset=utf-8><title>Game menu</title><style>"
    "body{margin:0;width:480px;height:360px;box-sizing:border-box;padding:20px "
    "28px;overflow:hidden;font-family:sans-serif;"
    "color:#e8ecff;background:linear-gradient(160deg,#2c3e7a,#141b36);border-radius:20px}"
    "h1{margin:0;font-size:30px;letter-spacing:.5px}"
    ".sub{margin:2px 0 "
    "14px;font-size:11px;color:#9fb0e6;text-transform:uppercase;letter-spacing:1.5px}"
    "button{display:block;width:280px;margin:0 0 10px;padding:9px 16px;font:600 15px "
    "sans-serif;color:#fff;"
    "background:#3b5bdb;border:0;border-radius:8px;text-align:left;cursor:pointer}"
    "button:hover{background:#5c7cfa}"
    "button:active{background:#2b44b0;transform:translateY(1px)}"
    ".bar{width:280px;height:10px;margin-top:12px;border-radius:5px;background:#0b1028}"
    ".bar "
    "i{display:block;height:100%;width:64%;border-radius:5px;background:linear-gradient(90deg,#"
    "63e6be,#69db7c)}"
    "p{margin:14px 0 0;width:340px;font-size:14px;line-height:1.4;color:#d5dcf5}"
    ".hint{margin-top:8px;font-size:11px;color:#8d9bd0}"
    "</style><h1>Ember Quest</h1><div class=sub>Main menu</div>"
    "<button>Continue</button><button>New game</button><button>Options</button>"
    "<div class=bar><i></i></div>"
    "<p>Restore the last checkpoint. The town gate opens at dawn.</p>"
    "<div class=hint>Loading 64 percent. Press Esc to return.</div>";

static const char k_terminal_page[] =
    "<!doctype html><meta charset=utf-8><title>Terminal</title><style>"
    "html,body{margin:0;background:#0b0f0b}"
    "body{width:480px;height:320px;box-sizing:border-box;padding:12px "
    "14px;overflow:hidden;font:13px/18px monospace;color:#8fe38f}"
    ".dim{color:#4f7f4f}.warn{color:#e8c46a}"
    ".cur{display:inline-block;width:8px;height:14px;margin-left:2px;background:#8fe38f;vertical-"
    "align:-2px;"
    "animation:blink 1s steps(1) infinite}"
    "@keyframes blink{50%{opacity:0}}"
    ".track{width:440px;height:4px;margin:6px 0 10px;background:#1a2a1a;overflow:hidden}"
    ".track i{display:block;width:60px;height:100%;background:#8fe38f;animation:sweep 1.6s linear "
    "infinite}"
    "@keyframes sweep{from{transform:translateX(-60px)}to{transform:translateX(440px)}}"
    "</style><div>$ ember --status</div>"
    "<div class=dim>engine   ok    scale 1.0   raster adaptive</div>"
    "<div class=dim>fonts    3 registered, 1 system</div>"
    "<div class=warn>cache    cold: 0 of 128 glyph pages</div>"
    "<div class=track><i></i></div>"
    "<div>$ tail -f frames.log</div>"
    "<div class=dim>frame 00041  layout 0.00 ms  paint 0.38 ms</div>"
    "<div class=dim>frame 00042  layout 0.00 ms  paint 0.31 ms</div>"
    "<div class=dim>frame 00043  layout 0.00 ms  paint 0.29 ms</div>"
    "<div>$ <span class=cur></span></div>";

/* A panel: its view, its rectangle in the atlas, its place in the world and its raster state. */
typedef struct panel
{
    emb_view *view;
    const char *name;
    uint32_t css_w, css_h;  /* the view's size in CSS px */
    uint32_t slot_x;        /* the left edge of its slot in the atlas (the top is 0) */
    float limit;            /* the largest raster scale whose device size fits the slot */
    float raster;           /* the current raster scale */
    bool fresh;             /* the next draw is a full one: the first, or after a raster change */
    bool redrew;            /* the last atlas draw recorded a picture */
    uint32_t dev_w, dev_h;  /* the view's device size now */
    float cx, cy, cz;       /* the panel's centre in the world */
    float angle;            /* rotation about Y, radians */
    float world_w, world_h; /* its size in world units */
    float model[16];        /* unit square -> world quad, column-major */
} panel;

/* What each draw of host_panel.slang reads: mirrors its cbuffer PanelParams (96 bytes). */
typedef struct panel_params
{
    float mvp[16];
    float uv_rect[4];
    float mode[4];
} panel_params;

/* Column-major 4x4 matrices and small vector helpers. */

static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul(float out[16], const float a[16], const float b[16])
{
    float t[16];
    for (int c = 0; c < 4; c++)
    {
        for (int r = 0; r < 4; r++)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
            {
                s += a[k * 4 + r] * b[c * 4 + k];
            }
            t[c * 4 + r] = s;
        }
    }
    memcpy(out, t, sizeof t);
}

static void mat4_translate(float m[16], float x, float y, float z)
{
    mat4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void mat4_scale(float m[16], float x, float y, float z)
{
    mat4_identity(m);
    m[0] = x;
    m[5] = y;
    m[10] = z;
}

static void mat4_rot_y(float m[16], float angle)
{
    mat4_identity(m);
    float c = cosf(angle), s = sinf(angle);
    m[0] = c;
    m[2] = -s;
    m[8] = s;
    m[10] = c;
}

/* A perspective projection with OpenGL conventions (depth in [-1, 1]). The scene uses it only for
 * x, y and the ray; no depth test is needed for the panels, which never overlap in depth. */
static void mat4_perspective(float m[16], float fovy, float aspect, float znear, float zfar)
{
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fovy * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = 2.0f * zfar * znear / (znear - zfar);
}

static void vec3_norm(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f)
    {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static float vec3_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void mat4_look_at(float m[16], const float eye[3], const float target[3], const float up[3])
{
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    vec3_norm(f);
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    vec3_norm(s);
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    mat4_identity(m);
    m[0] = s[0];
    m[4] = s[1];
    m[8] = s[2];
    m[1] = u[0];
    m[5] = u[1];
    m[9] = u[2];
    m[2] = -f[0];
    m[6] = -f[1];
    m[10] = -f[2];
    m[12] = -vec3_dot(s, eye);
    m[13] = -vec3_dot(u, eye);
    m[14] = vec3_dot(f, eye);
}
/* The inverse of a general 4x4 matrix by Gauss-Jordan elimination with partial pivoting. False when it is singular. */
static bool mat4_inverse(float out[16], const float m[16])
{
    float a[4][8];
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            a[r][c] = m[c * 4 + r];
            a[r][4 + c] = r == c ? 1.0f : 0.0f;
        }
    }
    for (int col = 0; col < 4; col++)
    {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
        {
            if (fabsf(a[r][col]) > fabsf(a[piv][col]))
            {
                piv = r;
            }
        }
        if (fabsf(a[piv][col]) < 1e-12f)
        {
            return false;
        }
        for (int c = 0; c < 8; c++)
        {
            float t = a[col][c];
            a[col][c] = a[piv][c];
            a[piv][c] = t;
        }
        float p = 1.0f / a[col][col];
        for (int c = 0; c < 8; c++)
        {
            a[col][c] *= p;
        }
        for (int r = 0; r < 4; r++)
        {
            if (r != col && a[r][col] != 0.0f)
            {
                float f = a[r][col];
                for (int c = 0; c < 8; c++)
                {
                    a[r][c] -= f * a[col][c];
                }
            }
        }
    }
    for (int r = 0; r < 4; r++)
    {
        for (int c = 0; c < 4; c++)
        {
            out[c * 4 + r] = a[r][4 + c];
        }
    }
    return true;
}

static void mat4_transform(const float m[16], const float v[4], float out[4])
{
    float t[4];
    for (int r = 0; r < 4; r++)
    {
        t[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    }
    memcpy(out, t, sizeof t);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------ logging and shaders */

static void print_log(void *user, emb_log_level level, const char *message)
{
    (void)user;
    static const char *const names[] = {"debug", "info", "warn", "error"};
    int idx = (int)level;
    if (idx < 0 || idx > (int)EMB_LOG_ERROR)
    {
        idx = (int)EMB_LOG_ERROR;
    }
    fprintf(stderr, "[ember %s] %s\n", names[idx], message);
}

/* A shader of the program's stage in the first format the device takes. nullptr when there is none. */
static SDL_GPUShader *make_shader(SDL_GPUDevice *dev, const embi_shader_stage *st, SDL_GPUShaderStage stage)
{
    SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev);
    const embi_shader_blob *blob = nullptr;
    SDL_GPUShaderFormat fmt = 0;
    if ((formats & SDL_GPU_SHADERFORMAT_DXIL) != 0 && st->dxil.code != nullptr)
    {
        blob = &st->dxil;
        fmt = SDL_GPU_SHADERFORMAT_DXIL;
    }
    else if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) != 0 && st->spirv.code != nullptr)
    {
        blob = &st->spirv;
        fmt = SDL_GPU_SHADERFORMAT_SPIRV;
    }
    else if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0 && st->msl.code != nullptr)
    {
        blob = &st->msl;
        fmt = SDL_GPU_SHADERFORMAT_MSL;
    }
    if (blob == nullptr)
    {
        return nullptr;
    }
    embi_gpu_blob_view bv = embi_gpu_blob_open(blob); /* the code, expanded when the build packs its blobs */
    if (bv.code == nullptr)
    {
        return nullptr;
    }
    SDL_GPUShaderCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.code = bv.code;
    ci.code_size = bv.size;
    ci.entrypoint = blob->entry;
    ci.format = fmt;
    ci.stage = stage;
    ci.num_samplers = st->num_samplers;
    ci.num_storage_textures = st->num_storage_textures;
    ci.num_storage_buffers = st->num_storage_buffers;
    ci.num_uniform_buffers = st->num_uniform_buffers;
    SDL_GPUShader *sh = SDL_CreateGPUShader(dev, &ci);
    embi_gpu_blob_close(&bv);
    return sh;
}

/* The panel pipeline: no vertex buffers, premultiplied blending over the target. */
static SDL_GPUGraphicsPipeline *make_pipeline(SDL_GPUDevice *dev, SDL_Window *win)
{
    SDL_GPUShader *vs = make_shader(dev, &embi_shader_host_panel.vs, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fs = make_shader(dev, &embi_shader_host_panel.fs, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (vs == nullptr || fs == nullptr)
    {
        fprintf(stderr, "world_panel: no shader for this device (%s)\n", SDL_GetError());
        SDL_ReleaseGPUShader(dev, vs);
        SDL_ReleaseGPUShader(dev, fs);
        return nullptr;
    }
    SDL_GPUColorTargetDescription ctd;
    memset(&ctd, 0, sizeof ctd);
    ctd.format = SDL_GetGPUSwapchainTextureFormat(dev, win);
    ctd.blend_state.enable_blend = true;
    ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE; /* the pictures are premultiplied */
    ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.vertex_shader = vs;
    pi.fragment_shader = fs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info.num_color_targets = 1;
    pi.target_info.color_target_descriptions = &ctd;
    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
    if (p == nullptr)
    {
        fprintf(stderr, "world_panel: the pipeline could not be created (%s)\n", SDL_GetError());
    }
    SDL_ReleaseGPUShader(dev, vs);
    SDL_ReleaseGPUShader(dev, fs);
    return p;
}

/* ------------------------------------------------------------------ raster scale */

/* Projects a world point to window px (top-left origin). False when it is behind the camera. */
static bool project(const float vp[16], const float world[4], float pw, float ph, float out[2])
{
    float clip[4];
    mat4_transform(vp, world, clip);
    if (clip[3] <= 1e-4f)
    {
        return false;
    }
    out[0] = (clip[0] / clip[3] * 0.5f + 0.5f) * pw;
    out[1] = (0.5f - clip[1] / clip[3] * 0.5f) * ph;
    return true;
}

/* The device px per CSS px that makes the panel's text one device px per screen px: the panel's
 * height on screen over its CSS height. Negative when the panel is not in front of the camera. */
static float desired_raster(const panel *p, const float vp[16], float pw, float ph)
{
    const float top_local[4] = {0.5f, 0.0f, 0.0f, 1.0f};
    const float bottom_local[4] = {0.5f, 1.0f, 0.0f, 1.0f};
    float top_world[4], bottom_world[4];
    mat4_transform(p->model, top_local, top_world);
    mat4_transform(p->model, bottom_local, bottom_world);
    float a[2], b[2];
    if (!project(vp, top_world, pw, ph, a) || !project(vp, bottom_world, pw, ph, b))
    {
        return -1.0f;
    }
    return fabsf(b[1] - a[1]) / (float)p->css_h;
}

/* Moves the raster scale to the step nearest desired, but only when desired has left the current
 * step by more than an eighth of an octave (hysteresis): a panel that breathes around a step keeps
 * its raster and does not redraw every frame. The steps are powers of 2^(1/4), and the scale never
 * makes the device size larger than the slot. */
static void update_raster(panel *p, float desired)
{
    if (desired < 0.0f)
    {
        return;
    }
    float d = clampf(desired, 0.25f, p->limit);
    if (fabsf(log2f(d) - log2f(p->raster)) <= 0.125f)
    {
        return;
    }
    float q = exp2f(roundf(4.0f * log2f(d)) / 4.0f);
    if (q > p->limit)
    {
        q = p->limit;
    }
    if (q == p->raster)
    {
        return;
    }
    p->raster = q;
    emb_view_set_raster_scale(p->view, q);
    p->fresh = true; /* the rectangle changes size: the next atlas draw is a full one */
}

/* ------------------------------------------------------------------ atlas */

static void clear_atlas(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *atlas)
{
    SDL_GPUColorTargetInfo ct;
    memset(&ct, 0, sizeof ct);
    ct.texture = atlas;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_EndGPURenderPass(pass);
}

/* Draws one panel into its slot. The rectangle is the view's device size at the slot's left edge,
 * top 0. The first draw and the draw after a raster change are full; otherwise only what changed is
 * drawn again. */
static bool draw_panel_to_atlas(panel *p, SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *atlas)
{
    emb_render_target t;
    memset(&t, 0, sizeof t);
    t.size = sizeof t;
    t.texture = atlas;
    t.texture_w = ATLAS_W;
    t.texture_h = ATLAS_H;
    t.x = (int32_t)p->slot_x;
    t.y = 0;
    t.flags = p->fresh ? 0u : EMB_RENDER_PRESERVED;
    p->redrew = emb_view_render_to(p->view, cmd, &t);
    p->fresh = false;
    emb_view_device_size(p->view, &p->dev_w, &p->dev_h);
    return p->redrew;
}

/* ------------------------------------------------------------------ input */

/* The panel under the window point (mx, my) in device px of the window (pw x ph), or -1. Casts the
 * ray through the inverse view-projection, maps it into each panel's unit square, and returns the
 * nearest hit (the ray parameter t in [0, 1] along the near-to-far segment, which affine maps
 * keep). css receives the hit in CSS px of that panel's view. */
static int pick_panel(const panel *ps, const float inv_vp[16], float mx, float my, float pw, float ph, float css[2])
{
    float nx = mx / pw * 2.0f - 1.0f, ny = 1.0f - my / ph * 2.0f;
    float near_ndc[4] = {nx, ny, -1.0f, 1.0f}, far_ndc[4] = {nx, ny, 1.0f, 1.0f};
    float o4[4], f4[4];
    mat4_transform(inv_vp, near_ndc, o4);
    mat4_transform(inv_vp, far_ndc, f4);
    const float o[3] = {o4[0] / o4[3], o4[1] / o4[3], o4[2] / o4[3]};
    const float f[3] = {f4[0] / f4[3], f4[1] / f4[3], f4[2] / f4[3]};
    int best = -1;
    float best_t = 2.0f;
    for (int i = 0; i < NPANELS; i++)
    {
        float inv_model[16];
        if (!mat4_inverse(inv_model, ps[i].model))
        {
            continue;
        }
        float lo4[4] = {o[0], o[1], o[2], 1.0f}, lf4[4] = {f[0], f[1], f[2], 1.0f}, lo[4], lf[4];
        mat4_transform(inv_model, lo4, lo);
        mat4_transform(inv_model, lf4, lf);
        float dz = lf[2] - lo[2];
        if (fabsf(dz) < 1e-9f)
        {
            continue;
        }
        float t = -lo[2] / dz; /* where the segment crosses the panel's plane z = 0 */
        if (t < 0.0f || t > 1.0f || t >= best_t)
        {
            continue;
        }
        float u = lo[0] + (lf[0] - lo[0]) * t, v = lo[1] + (lf[1] - lo[1]) * t;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        {
            continue;
        }
        best = i;
        best_t = t;
        css[0] = u * (float)ps[i].css_w;
        css[1] = v * (float)ps[i].css_h;
    }
    return best;
}

/* ------------------------------------------------------------------ main */

static void setup_panel(panel *p, emb_view *view)
{
    p->view = view;
    p->limit = floorf(fminf((float)SLOT_W / (float)p->css_w, (float)ATLAS_H / (float)p->css_h) * 16.0f) / 16.0f;
    p->raster = 1.0f;
    p->fresh = true;
    emb_view_device_size(view, &p->dev_w, &p->dev_h);
}

int main(int argc, char **argv)
{
    long frames_limit = -1;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        {
            frames_limit = strtol(argv[++i], nullptr, 10);
        }
        else
        {
            fprintf(stderr, "usage: %s [--frames N]\n", argv[0]);
            return 2;
        }
    }

    panel ps[NPANELS] = {
        {.name = "menu",
         .css_w = 480,
         .css_h = 360,
         .slot_x = 0,
         .cx = -1.1f,
         .cy = 0.0f,
         .cz = 0.0f,
         .angle = 0.0f,
         .world_w = 2.0f,
         .world_h = 1.5f},
        {.name = "terminal",
         .css_w = 480,
         .css_h = 320,
         .slot_x = SLOT_W + GUTTER,
         .cx = 1.2f,
         .cy = -0.1f,
         .cz = -0.9f,
         .angle = 0.5236f,
         .world_w = 2.0f,
         .world_h = 1.3333f},
    };

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    int status = 1;
    SDL_Window *win = SDL_CreateWindow("Ember world panel", WIN_W, WIN_H, 0);
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (win == nullptr || dev == nullptr || !SDL_ClaimWindowForGPUDevice(dev, win))
    {
        fprintf(stderr, "window or device: %s\n", SDL_GetError());
        if (dev != nullptr)
        {
            SDL_DestroyGPUDevice(dev);
        }
        if (win != nullptr)
        {
            SDL_DestroyWindow(win);
        }
        SDL_Quit();
        return 1;
    }

    SDL_GPUGraphicsPipeline *pipe = make_pipeline(dev, win);
    SDL_GPUTexture *atlas = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    if (pipe != nullptr)
    {
        SDL_GPUTextureCreateInfo tci;
        memset(&tci, 0, sizeof tci);
        tci.type = SDL_GPU_TEXTURETYPE_2D;
        tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER; /* mip generation needs both */
        tci.width = ATLAS_W;
        tci.height = ATLAS_H;
        tci.layer_count_or_depth = 1;
        tci.num_levels = ATLAS_LEVELS;
        atlas = SDL_CreateGPUTexture(dev, &tci);

        SDL_GPUSamplerCreateInfo sci;
        memset(&sci, 0, sizeof sci);
        sci.min_filter = SDL_GPU_FILTER_LINEAR;
        sci.mag_filter = SDL_GPU_FILTER_LINEAR;
        sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR; /* trilinear */
        sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sci.max_lod = (float)ATLAS_LEVELS;
        sampler = SDL_CreateGPUSampler(dev, &sci);
    }

    emb_runtime_desc rd;
    memset(&rd, 0, sizeof rd);
    rd.size = sizeof rd;
    rd.log = print_log;
    emb_runtime *rt = (atlas != nullptr && sampler != nullptr) ? emb_runtime_create(&rd) : nullptr;
    if (rt != nullptr)
    {
        bool ok = true;
        for (int i = 0; i < NPANELS && ok; i++)
        {
            emb_view_desc vd;
            memset(&vd, 0, sizeof vd);
            vd.size = sizeof vd;
            vd.mode = EMB_VIEW_OFFSCREEN;
            vd.width = ps[i].css_w;
            vd.height = ps[i].css_h;
            vd.scale = 1.0f;
            vd.device = dev;
            vd.flags = EMB_VIEW_NO_TEXTURE; /* drawn only into the atlas: the view makes no texture
                                               of its own */
            vd.transparent = i == 0;        /* the menu's rounded corners show the dark background */
            emb_view *view = emb_view_create(rt, &vd);
            const char *html = i == 0 ? k_menu_page : k_terminal_page;
            size_t len = i == 0 ? sizeof k_menu_page - 1 : sizeof k_terminal_page - 1;
            if (view == nullptr || !emb_view_load_html(view, html, len, nullptr))
            {
                fprintf(stderr, "world_panel: the %s page could not be shown\n", ps[i].name);
                ok = false;
                break;
            }
            setup_panel(&ps[i], view);
        }
        status = ok ? 0 : 1;
    }

    const uint64_t t0 = SDL_GetTicks();
    uint64_t last_tick = t0;
    long frame = 0;
    long last_second = -1;
    float user_dist = 0.0f; /* the camera's offset from its base distance: W and S, and the wheel
                               over empty space */
    int hover = -1;         /* the panel under the pointer after the last frame, or -1 */
    float hover_css[2] = {0.0f, 0.0f};
    int press = -1; /* the panel that got the last left-button press */
    bool quit = status != 0;

    while (!quit && (frames_limit < 0 || frame < frames_limit))
    {
        const uint64_t now = SDL_GetTicks();
        const float dt = clampf((float)(now - last_tick) / 1000.0f, 0.0f, 0.1f);
        last_tick = now;
        const float seconds = (float)(now - t0) / 1000.0f;

        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            switch (ev.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE)
                {
                    quit = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT && hover >= 0)
                {
                    press = hover;
                    emb_view_mouse_button(ps[hover].view, hover_css[0], hover_css[1], EMB_BUTTON_PRIMARY, true, 0);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                {
                    int release_at = press >= 0 ? press : hover;
                    if (release_at >= 0)
                    {
                        emb_view_mouse_button(ps[release_at].view, hover_css[0], hover_css[1], EMB_BUTTON_PRIMARY,
                                              false, 0);
                    }
                    press = -1;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (hover >= 0)
                {
                    emb_view_wheel(ps[hover].view, hover_css[0], hover_css[1], ev.wheel.x * WHEEL_PX,
                                   -ev.wheel.y * WHEEL_PX);
                }
                else
                {
                    user_dist -= ev.wheel.y * DOLLY_WHEEL;
                }
                break;
            default:
                break;
            }
        }
        if (quit)
        {
            break;
        }
        const bool *keys = SDL_GetKeyboardState(nullptr);
        if (keys[SDL_SCANCODE_W])
        {
            user_dist -= DOLLY_KEY_SPEED * dt;
        }
        if (keys[SDL_SCANCODE_S])
        {
            user_dist += DOLLY_KEY_SPEED * dt;
        }

        /* The camera: the base distance breathes between 1.5 and 4.5, the user's offset adds to it.
         */
        int win_w = 0, win_h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(win, &win_w, &win_h);
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        if (pw <= 0 || ph <= 0)
        {
            pw = WIN_W;
            ph = WIN_H;
        }
        const float dist = clampf(3.0f + 1.5f * sinf(seconds * 0.35f) + user_dist, 1.2f, 9.0f);
        const float eye[3] = {0.0f, 0.35f, dist}, target[3] = {0.0f, 0.0f, 0.0f}, up[3] = {0.0f, 1.0f, 0.0f};
        float view_m[16], proj[16], vp[16], inv_vp[16];
        mat4_look_at(view_m, eye, target, up);
        mat4_perspective(proj, CAMERA_FOV, (float)pw / (float)ph, 0.1f, 100.0f);
        mat4_mul(vp, proj, view_m);
        mat4_inverse(inv_vp, vp);

        for (int i = 0; i < NPANELS; i++)
        {
            panel *p = &ps[i];
            /* model = centre * rotation about Y * size (y negated: the unit square's top edge is
             * the panel's top) * centring */
            float place[16], rot[16], size[16], centre[16];
            mat4_translate(place, p->cx, p->cy, p->cz);
            mat4_rot_y(rot, p->angle);
            mat4_scale(size, p->world_w, -p->world_h, 1.0f);
            mat4_translate(centre, -0.5f, -0.5f, 0.0f);
            mat4_mul(p->model, place, rot);
            mat4_mul(p->model, p->model, size);
            mat4_mul(p->model, p->model, centre);
        }

        /* Hover: the pointer's ray picks the panel it hits, and the view under it gets the pointer
         * in CSS px. */
        int new_hover = -1;
        float mouse_x = 0.0f, mouse_y = 0.0f;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        if (SDL_GetMouseFocus() == win && win_w > 0)
        {
            float scale = (float)pw / (float)win_w;
            new_hover = pick_panel(ps, inv_vp, mouse_x * scale, mouse_y * scale, (float)pw, (float)ph, hover_css);
        }
        if (new_hover != hover && hover >= 0)
        {
            emb_view_mouse_leave(ps[hover].view);
        }
        hover = new_hover;
        if (hover >= 0)
        {
            emb_view_mouse_move(ps[hover].view, hover_css[0], hover_css[1], 0);
        }

        /* Raster scale: each panel's screen size over its CSS size, quantized with hysteresis. */
        for (int i = 0; i < NPANELS; i++)
        {
            update_raster(&ps[i], desired_raster(&ps[i], vp, (float)pw, (float)ph));
        }

        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (cmd == nullptr)
        {
            break;
        }
        if (frame == 0)
        {
            clear_atlas(cmd, atlas);
        }
        bool any = false;
        for (int i = 0; i < NPANELS; i++)
        {
            any = draw_panel_to_atlas(&ps[i], cmd, atlas) || any;
        }
        if (any)
        {
            /* Mip level 0 is what the views drew; the chain is made from it. The panels sample the
             * chain (trilinear). */
            SDL_GenerateMipmapsForGPUTexture(cmd, atlas);
        }

        SDL_GPUTexture *sw = nullptr;
        uint32_t sw_w = 0, sw_h = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &sw, &sw_w, &sw_h) && sw != nullptr)
        {
            SDL_GPUColorTargetInfo ct;
            memset(&ct, 0, sizeof ct);
            ct.texture = sw;
            ct.clear_color = (SDL_FColor){0.02f, 0.02f, 0.04f, 1.0f};
            ct.load_op = SDL_GPU_LOADOP_CLEAR;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, pipe);
            SDL_GPUTextureSamplerBinding tsb;
            memset(&tsb, 0, sizeof tsb);
            tsb.texture = atlas;
            tsb.sampler = sampler;
            SDL_BindGPUFragmentSamplers(pass, 0, &tsb, 1);

            /* The gradient behind the panels: a full-screen quad with the gradient mode. */
            panel_params bg;
            memset(&bg, 0, sizeof bg);
            mat4_identity(bg.mvp);
            bg.mvp[0] = 2.0f; /* the unit square to clip space, with v pointing down */
            bg.mvp[5] = -2.0f;
            bg.mvp[12] = -1.0f;
            bg.mvp[13] = 1.0f;
            bg.uv_rect[2] = bg.uv_rect[3] = 1.0f;
            bg.mode[0] = 1.0f;
            SDL_PushGPUVertexUniformData(cmd, 0, &bg, sizeof bg);
            SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);

            /* The panels: each samples its own slot, the sub-rectangle of its device size (the rest
             * of the slot is unused). */
            for (int i = 0; i < NPANELS; i++)
            {
                const panel *p = &ps[i];
                panel_params pp;
                memset(&pp, 0, sizeof pp);
                mat4_mul(pp.mvp, vp, p->model);
                pp.uv_rect[0] = (float)p->slot_x / (float)ATLAS_W;
                pp.uv_rect[1] = 0.0f;
                pp.uv_rect[2] = (float)(p->slot_x + p->dev_w) / (float)ATLAS_W;
                pp.uv_rect[3] = (float)p->dev_h / (float)ATLAS_H;
                SDL_PushGPUVertexUniformData(cmd, 0, &pp, sizeof pp);
                SDL_DrawGPUPrimitives(pass, 6, 1, 0, 0);
            }
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(cmd);

        const long second = (long)seconds;
        if (frame == 0 || second != last_second)
        {
            last_second = second;
            printf("frame %ld  t=%.1fs  menu: raster %.3f device %ux%u %s  |  terminal: raster "
                   "%.3f device %ux%u %s\n",
                   frame, (double)seconds, (double)ps[0].raster, ps[0].dev_w, ps[0].dev_h,
                   ps[0].redrew ? "redrew" : "kept", (double)ps[1].raster, ps[1].dev_w, ps[1].dev_h,
                   ps[1].redrew ? "redrew" : "kept");
            fflush(stdout);
        }
        frame++;
    }

    printf("world_panel: %ld frames\n", frame);
    fflush(stdout);

    for (int i = 0; i < NPANELS; i++)
    {
        emb_view_destroy(ps[i].view);
    }
    emb_runtime_destroy(rt);
    SDL_ReleaseGPUSampler(dev, sampler);
    SDL_ReleaseGPUTexture(dev, atlas);
    if (pipe != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(dev, pipe);
    }
    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return status;
}
