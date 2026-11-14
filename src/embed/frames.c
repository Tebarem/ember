/*
 * iframes (IF1, frames.h): the child views of a view's iframes, their lifetime, loading, viewport,
 * update, draw, damage and the hooks of the bindings (contentWindow, parent, top, postMessage between frames).
 *
 * The frames of a document are found when it has been parsed (embi_frames_doc_ready) and when a subtree holding an iframe
 * is inserted later (a DOM observer); the view's next update makes their child views. A removed iframe's frame is marked
 * dead by the observer and destroyed by the next update (its realm, timers and pending messages go with it). A change of
 * src or srcdoc loads the frame again. Loads run in the parent's update, never during its parse, so the parent never waits.
 *
 * Every view (a top-level one too) has a window id for its current document. The bindings know other windows only by id:
 * a message names its target by id, the id is looked up when the message is posted and again when its task runs, and a
 * window that navigated or went away drops it.
 */
#include "embed/frames.h"

#include "embed_int.h"

#include "bindings/bind.h"
#include "dom/dom.h"
#include "embed/input_int.h"
#include "embed/netload.h"
#include "embed/resource_int.h"
#include "embed/script_int.h"
#include "embed/url.h"
#include "js/js.h"
#include "layout/layout.h"
#include "loader/loader.h"
#include "loader/msgport.h"
#include "bindings/loader_host.h"

#include <math.h>
#include <string.h>

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_stdinc.h>

struct embi_frames {
    emb_view *view;
    /* As a frame (parent != nullptr). */
    emb_view *parent;
    embi_element *el;     /* the iframe in the parent's document; nullptr once it left it (the frame is dead) */
    uint32_t depth;       /* 0 for a top-level view */
    bool load_pending;    /* load (again) at the next update: a new frame, or a src or srcdoc change */
    bool loaded;          /* a load ran once (the viewport changes after it fire resize) */
    bool changed;         /* the last update changed the picture and it was not drawn yet */
    bool complete_sent;   /* the parent's iframe got (or will get) its load event for the current document */
    char *nav_url;        /* a navigation from inside the frame, loaded instead of src (SDL_malloc'd) */
    uint32_t vw, vh;      /* the viewport given to the frame (CSS px), 0 before the first */
    /* As a view with a document. */
    uint32_t win_id;      /* the window of the current document (0: none yet) */
    bool initial_win;     /* win_id names the frame's initial empty document: the first document loaded keeps it */
    emb_view **kids;
    uint32_t nkids, capkids;
    embi_document *obs_doc; /* the document the observer watches */
    bool rescan;          /* an iframe may have been inserted: look for iframes without a frame */
    embi_frame_route route;
    embi_mports *ports;   /* at the top: the MessagePort registry the realms of the whole tree share (one reference) */
};

static SDL_AtomicInt g_win_ids;

static uint32_t new_win_id(void)
{
    uint32_t id = (uint32_t)SDL_AtomicIncRef(&g_win_ids) + 1u;
    return id != 0 ? id : (uint32_t)SDL_AtomicIncRef(&g_win_ids) + 1u;
}

static embi_frames *fr_of(emb_view *v)
{
    if (v->frames == nullptr) {
        embi_frames *f = SDL_calloc(1, sizeof *f);
        if (f == nullptr) {
            return nullptr;
        }
        f->view = v;
        v->frames = f;
    }
    return v->frames;
}

bool embi_frames_adopt(emb_view *v, emb_view *parent)
{
    embi_frames *f = fr_of(v);
    embi_frames *pf = fr_of(parent);
    if (f == nullptr || pf == nullptr) {
        return false;
    }
    f->parent = parent;
    f->depth = pf->depth + 1;
    return true;
}

static bool is_iframe(const embi_node *n)
{
    return n != nullptr && embi_is_element(n) && n->extra == EMBI_NS_HTML && n->name == EMBI_ATOM_iframe;
}

emb_view *embi_frames_parent(const emb_view *v)
{
    return v != nullptr && v->frames != nullptr ? v->frames->parent : nullptr;
}

emb_view *embi_frames_root(emb_view *v)
{
    while (embi_frames_parent(v) != nullptr) {
        v = v->frames->parent;
    }
    return v;
}

embi_element *embi_frames_element(const emb_view *v)
{
    return v != nullptr && v->frames != nullptr ? v->frames->el : nullptr;
}

embi_frame_route *embi_frames_route(emb_view *v)
{
    embi_frames *f = fr_of(v);
    return f != nullptr ? &f->route : nullptr;
}

bool embi_frames_any(const emb_view *v)
{
    return v != nullptr && v->frames != nullptr && v->frames->nkids > 0;
}

bool embi_frames_live(const emb_view *v)
{
    const embi_frames *f = v->frames;
    return f == nullptr || f->parent == nullptr || f->el != nullptr;
}

emb_view *embi_frames_child(const emb_view *v, uint32_t i)
{
    return v != nullptr && v->frames != nullptr && i < v->frames->nkids ? v->frames->kids[i] : nullptr;
}

/* The frame of iframe el among v's frames (live ones only). */
static emb_view *kid_of(const embi_frames *f, const embi_element *el)
{
    if (f == nullptr || el == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < f->nkids; i++) {
        if (f->kids[i]->frames->el == el) {
            return f->kids[i];
        }
    }
    return nullptr;
}

/* The view with window id below v (v included), nullptr when none. */
static emb_view *find_win(emb_view *v, uint32_t id)
{
    embi_frames *f = v->frames;
    if (f == nullptr || id == 0) {
        return nullptr;
    }
    if (f->win_id == id && embi_frames_live(v)) {
        return v;
    }
    for (uint32_t i = 0; i < f->nkids; i++) {
        emb_view *r = find_win(f->kids[i], id);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

void embi_frames_route_forget(emb_view *v, emb_view *kid)
{
    for (emb_view *p = v; p != nullptr; p = embi_frames_parent(p)) {
        embi_frames *f = p->frames;
        if (f == nullptr) {
            continue;
        }
        embi_frame_route *r = &f->route;
        if (r->hover == kid) {
            r->hover = nullptr;
        }
        if (r->press == kid) {
            r->press = nullptr;
        }
        if (r->focus == kid) {
            r->focus = nullptr;
        }
        for (uint32_t i = 0; i < EMBI_FRAMES_TOUCHES; i++) {
            if (r->touch[i].kid == kid) {
                r->touch[i].kid = nullptr;
            }
        }
    }
}

/* Destroys frame i of v. */
static void kid_remove(emb_view *v, uint32_t i)
{
    embi_frames *f = v->frames;
    emb_view *kid = f->kids[i];
    memmove(&f->kids[i], &f->kids[i + 1], (size_t)(f->nkids - i - 1) * sizeof f->kids[0]);
    f->nkids--;
    embi_frames_route_forget(v, kid);
    emb_view_destroy(kid);
    v->paint_dirty = true; /* its picture leaves the parent's */
}

static void kids_clear(emb_view *v)
{
    embi_frames *f = v->frames;
    while (f != nullptr && f->nkids > 0) {
        kid_remove(v, f->nkids - 1);
    }
}

void embi_frames_destroy(emb_view *v)
{
    embi_frames *f = v->frames;
    if (f == nullptr) {
        return;
    }
    kids_clear(v);
    SDL_free(f->kids);
    SDL_free(f->nav_url);
    embi_mports_unref(f->ports); /* the realms still alive keep their references */
    SDL_free(f);
    v->frames = nullptr;
}

/* ------------------------------------------------------------------ the DOM observer */

/* Whether the subtree at n holds an iframe. */
static bool has_iframe(embi_node *n)
{
    if (is_iframe(n)) {
        return true;
    }
    for (embi_node *k = embi_next_in_tree(n, n); k != nullptr; k = embi_next_in_tree(n, k)) {
        if (is_iframe(k)) {
            return true;
        }
    }
    return false;
}

static void obs_inserted(void *ctx, embi_node *node)
{
    embi_frames *f = ctx;
    if (!f->rescan && has_iframe(node)) {
        f->rescan = true;
        f->view->paint_dirty = true; /* the next update makes the frame */
    }
}

/* A removed subtree: the frames of the iframes in it are dead (the next update destroys them). */
static void obs_removed(void *ctx, embi_node *node)
{
    embi_frames *f = ctx;
    for (uint32_t i = 0; i < f->nkids; i++) {
        embi_frames *kf = f->kids[i]->frames;
        if (kf->el == nullptr) {
            continue;
        }
        for (const embi_node *a = &kf->el->base; a != nullptr; a = a->parent) {
            if (a == node) {
                kf->el = nullptr;
                f->view->paint_dirty = true;
                break;
            }
        }
    }
}

static void obs_attr(void *ctx, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local, embi_dstr old, bool had)
{
    (void)ns_uri;
    (void)old;
    (void)had;
    embi_frames *f = ctx;
    if (ns != 0 || (local != EMBI_ATOM_src && local != EMBI_ATOM_srcdoc) || !is_iframe(&el->base)) {
        return;
    }
    emb_view *kid = kid_of(f, el);
    if (kid != nullptr) {
        embi_frames *kf = kid->frames;
        kf->load_pending = true; /* even the same value loads again */
        SDL_free(kf->nav_url);
        kf->nav_url = nullptr;
        f->view->paint_dirty = true;
    }
}

/* ------------------------------------------------------------------ document lifecycle */

void embi_frames_doc_begin(emb_view *v)
{
    embi_frames *f = fr_of(v);
    if (f == nullptr) {
        return;
    }
    kids_clear(v);
    memset(&f->route, 0, sizeof f->route);
    f->obs_doc = nullptr;
    f->rescan = false;
    f->complete_sent = false;
    if (f->initial_win && f->win_id != 0) {
        f->initial_win = false; /* the first document replaces the initial empty one: contentWindow stays the same */
        return;
    }
    f->win_id = new_win_id();
}

void embi_frames_doc_ready(emb_view *v)
{
    if (v == nullptr || v->doc == nullptr) {
        return;
    }
    embi_frames *f = fr_of(v);
    embi_document *dom = embi_doc_dom(v->doc);
    if (f == nullptr || dom == nullptr) {
        return;
    }
    embi_document_unobserve(dom, f);
    embi_observer obs;
    memset(&obs, 0, sizeof obs);
    obs.ctx = f;
    obs.node_inserted = obs_inserted;
    obs.node_removed = obs_removed;
    obs.attribute_changed = obs_attr;
    embi_document_observe(dom, &obs);
    f->obs_doc = dom;
    f->rescan = has_iframe(&dom->base);
}

typedef struct load_ev {
    emb_view *parent;
    uint32_t kid_win;
} load_ev;

static void load_ev_drop(void *user)
{
    SDL_free(user);
}

/* In the parent's loop: the load event of the iframe whose frame shows the window kid_win. */
static void load_ev_run(void *user)
{
    load_ev *e = user;
    emb_view *p = e->parent;
    embi_frames *f = p->frames;
    emb_view *kid = nullptr;
    for (uint32_t i = 0; f != nullptr && i < f->nkids; i++) {
        if (f->kids[i]->frames->win_id == e->kid_win && f->kids[i]->frames->el != nullptr) {
            kid = f->kids[i];
        }
    }
    SDL_free(e);
    if (kid == nullptr || p->doc == nullptr) {
        return;
    }
    embi_element *el = kid->frames->el;
    embi_script *s = embi_doc_script(p->doc);
    embi_bind_ctx *c = s != nullptr ? embi_script_bind_if(s) : nullptr;
    if (c == nullptr && s != nullptr && embi_el_has_attribute(el, EMBI_STR("onload"))) {
        c = embi_script_bind(s); /* an onload attribute is a listener with no script to make the realm */
    }
    if (c == nullptr || (el->base.flags & EMBI_NF_CONNECTED) == 0) {
        return;
    }
    embi_bind_fire_node_event(c, &el->base, "load");
    embi_js_run_jobs(c->rt);
}

void embi_frames_doc_complete(emb_view *v)
{
    embi_frames *kf = v != nullptr ? v->frames : nullptr;
    if (kf == nullptr || kf->parent == nullptr || kf->complete_sent || kf->el == nullptr) {
        return;
    }
    kf->complete_sent = true;
    emb_view *p = kf->parent;
    embi_script *s = p->doc != nullptr ? embi_doc_script(p->doc) : nullptr;
    load_ev *e = SDL_malloc(sizeof *e);
    if (s == nullptr || e == nullptr) {
        SDL_free(e);
        return;
    }
    e->parent = p;
    e->kid_win = kf->win_id;
    if (!embi_script_post_ex(s, load_ev_run, e, load_ev_drop)) {
        SDL_free(e);
    }
}

bool embi_frames_navigate(emb_view *v, embi_str url)
{
    embi_frames *kf = v != nullptr ? v->frames : nullptr;
    if (kf == nullptr || kf->parent == nullptr || url.len == 0) {
        return false;
    }
    char *u = SDL_strndup(url.ptr, url.len);
    if (u == nullptr) {
        return false;
    }
    SDL_free(kf->nav_url);
    kf->nav_url = u;
    kf->load_pending = true;
    kf->parent->paint_dirty = true;
    return true;
}

/* ------------------------------------------------------------------ geometry */

/* The content box of iframe el in v's viewport (CSS px, where it is painted: the transforms applied as a scale of the box,
 * the scroll offsets of its scrollers applied, a fixed frame kept still), and the scale. */
static bool content_box(emb_view *v, const embi_element *el, float box[4], float *sx, float *sy, float *cw, float *ch)
{
    embi_layout_engine *e = v->doc != nullptr ? embi_doc_layout_engine(v->doc) : nullptr;
    embi_rectf bb, xb;
    float bdr[4], pad[4], mg[4];
    if (e == nullptr || !embi_layout_border_box(e, el, &bb) || !embi_layout_box_edges(e, el, bdr, pad, mg)) {
        return false;
    }
    if (!embi_layout_painted_box_xf(e, el, &xb)) {
        xb = bb;
    }
    float w = bb.w - bdr[0] - bdr[2] - pad[0] - pad[2];
    float h = bb.h - bdr[1] - bdr[3] - pad[1] - pad[3];
    if (!(w > 0.0f) || !(h > 0.0f)) {
        return false;
    }
    float kx = bb.w > 0.0f ? xb.w / bb.w : 1.0f, ky = bb.h > 0.0f ? xb.h / bb.h : 1.0f;
    float ox = 0.0f, oy = 0.0f;
    embi_doc_root_offset(v->doc, &ox, &oy);
    box[0] = xb.x - ox + (bdr[0] + pad[0]) * kx;
    box[1] = xb.y - oy + (bdr[1] + pad[1]) * ky;
    box[2] = w * kx;
    box[3] = h * ky;
    *sx = kx > 0.0f ? kx : 1.0f;
    *sy = ky > 0.0f ? ky : 1.0f;
    *cw = w;
    *ch = h;
    return true;
}

emb_view *embi_frames_at(emb_view *v, float x, float y, float *lx, float *ly)
{
    embi_frames *f = v->frames;
    if (f == nullptr || f->nkids == 0 || v->doc == nullptr || embi_view_load_suspended(v)) {
        return nullptr;
    }
    embi_view_layout(v);
    embi_layout_engine *e = embi_doc_layout_engine(v->doc);
    embi_document *dom = embi_doc_dom(v->doc);
    if (e == nullptr || dom == nullptr) {
        return nullptr;
    }
    /* The hit test gives the topmost element, the parts outside its ancestors' clips excluded, and the point in the
     * element's own space (its transforms and the scroll offsets of its scrollers undone). */
    float local[2];
    embi_element *el = embi_layout_hit_test_local(e, dom, x, y, local);
    emb_view *kid = is_iframe(el != nullptr ? &el->base : nullptr) ? kid_of(f, el) : nullptr;
    if (kid == nullptr || kid->doc == nullptr) {
        return nullptr;
    }
    float bdr[4], pad[4], mg[4];
    float box[4], sx, sy, cw, ch;
    if (!content_box(v, el, box, &sx, &sy, &cw, &ch) || !embi_layout_box_edges(e, el, bdr, pad, mg)) {
        return nullptr;
    }
    float px, py;
    if (isfinite(local[0]) && isfinite(local[1])) {
        px = local[0] - bdr[0] - pad[0];
        py = local[1] - bdr[1] - pad[1];
    } else {
        px = (x - box[0]) / sx; /* unknown: the box's bounds stand in */
        py = (y - box[1]) / sy;
    }
    if (px < 0.0f || py < 0.0f || px >= cw || py >= ch) {
        return nullptr; /* the iframe's border or padding: the parent's */
    }
    *lx = px;
    *ly = py;
    return kid;
}

bool embi_frames_to_root(emb_view *v, float x, float y, float *rx, float *ry)
{
    for (; embi_frames_parent(v) != nullptr; v = v->frames->parent) {
        embi_frames *kf = v->frames;
        float box[4], sx, sy, cw, ch;
        if (kf->el == nullptr || !content_box(kf->parent, kf->el, box, &sx, &sy, &cw, &ch)) {
            return false;
        }
        x = box[0] + x * sx;
        y = box[1] + y * sy;
    }
    *rx = x;
    *ry = y;
    return true;
}

bool embi_frames_map(emb_view *kid, float x, float y, float *lx, float *ly)
{
    embi_frames *kf = kid->frames;
    if (kf == nullptr || kf->parent == nullptr || kf->el == nullptr) {
        return false;
    }
    float box[4], sx, sy, cw, ch;
    if (!content_box(kf->parent, kf->el, box, &sx, &sy, &cw, &ch)) {
        return false;
    }
    *lx = (x - box[0]) / sx;
    *ly = (y - box[1]) / sy;
    return true;
}

/* ------------------------------------------------------------------ loading */

static void load_blank(emb_view *kid, const char *url)
{
    (void)emb_view_load_html(kid, "", 0, url);
}

/* Loads the frame's document: the navigation from inside it, srcdoc, src, or an empty document. */
static void kid_load(emb_view *parent, emb_view *kid)
{
    embi_frames *kf = kid->frames;
    kf->load_pending = false;
    if (kf->el == nullptr) {
        return;
    }
    if (kf->depth > EMBI_FRAMES_MAX_DEPTH) {
        embi_rt_log(kid->rt, EMB_LOG_WARN, "iframe: frames nest deeper than %u levels; the frame is left empty",
                    EMBI_FRAMES_MAX_DEPTH);
        kid->net_doc_next = parent->net_doc;
        load_blank(kid, "about:blank");
        return;
    }
    char *url = kf->nav_url;
    kf->nav_url = nullptr;
    /* A frame inherits its parent's standing: a document inside a page from the network (srcdoc, data:, about:blank, or
     * where such a document navigated) is from the network too, and cannot load local resources. A network src raises the
     * flag itself when its response arrives. */
    kid->net_doc_next = parent->net_doc || (url != nullptr && kid->net_doc);
    embi_sb attr = {nullptr, 0, 0};
    if (url == nullptr && embi_doc_el_attr(kf->el, "srcdoc", &attr)) {
        embi_sb_cstr(&attr);
        (void)emb_view_load_html(kid, attr.data != nullptr ? attr.data : "", attr.len, "about:srcdoc");
        embi_sb_free(&attr);
        return;
    }
    embi_sb_clear(&attr);
    if (url == nullptr && embi_doc_el_attr(kf->el, "src", &attr) && attr.len > 0) {
        embi_str abs = embi_doc_resolve(parent->doc, (embi_str){attr.data, (uint32_t)attr.len});
        if (abs.len > 0) {
            url = SDL_strndup(abs.ptr, abs.len);
        }
    }
    embi_sb_free(&attr);
    if (url == nullptr) {
        load_blank(kid, "about:blank");
        return;
    }
    embi_str u = {url, (uint32_t)strlen(url)};
    embi_str scheme;
    bool ok = false;
    if (embi_url_scheme(u, &scheme) && embi_url_scheme_is(scheme, "about")) {
        load_blank(kid, url);
        ok = true;
    } else if (embi_netload_scheme_ok(parent, u)) {
        ok = emb_view_load_url(kid, url); /* a network page arrives later: the frame has no document meanwhile */
    }
    if (!ok) {
        embi_rt_log(kid->rt, EMB_LOG_WARN, "iframe: %s could not be loaded; the frame is left empty", url);
        kid->net_doc_next = parent->net_doc || kid->net_doc_next;
        load_blank(kid, "about:blank");
    }
    SDL_free(url);
}

/* ------------------------------------------------------------------ update, damage, draw */

static emb_view *kid_new(emb_view *v, embi_element *el)
{
    embi_frames *f = v->frames;
    if (f->nkids == f->capkids) {
        uint32_t cap = f->capkids ? f->capkids * 2 : 4;
        emb_view **k = SDL_realloc(f->kids, (size_t)cap * sizeof *k);
        if (k == nullptr) {
            return nullptr;
        }
        f->kids = k;
        f->capkids = cap;
    }
    emb_view *kid = embi_view_new_child(v);
    if (kid == nullptr) {
        return nullptr;
    }
    embi_frames *kf = kid->frames;
    kf->el = el;
    kf->load_pending = true;
    kf->win_id = new_win_id(); /* the initial empty document's window, which contentWindow gives until the load */
    kf->initial_win = true;
    f->kids[f->nkids++] = kid;
    v->paint_dirty = true; /* the next update loads it */
    return kid;
}

/* Makes frames for the connected iframes of v's document that have none. */
static void rescan(emb_view *v)
{
    embi_frames *f = v->frames;
    embi_document *dom = embi_doc_dom(v->doc);
    if (dom == nullptr) {
        return;
    }
    embi_node *root = &dom->base;
    for (embi_node *n = embi_next_in_tree(root, root); n != nullptr; n = embi_next_in_tree(root, n)) {
        if (is_iframe(n) && (n->flags & EMBI_NF_CONNECTED) != 0 && kid_of(f, embi_as_element(n)) == nullptr) {
            (void)kid_new(v, embi_as_element(n));
        }
    }
}

/* Fires resize at the frame's window (its viewport changed after its document loaded). */
static void fire_resize(emb_view *kid)
{
    embi_script *s = kid->doc != nullptr ? embi_doc_script(kid->doc) : nullptr;
    embi_bind_ctx *c = s != nullptr ? embi_script_bind_if(s) : nullptr;
    if (c != nullptr) {
        embi_bind_fire_window_event(c, "resize");
        embi_js_run_jobs(c->rt);
    }
}

bool embi_frames_update(emb_view *v)
{
    embi_frames *f = v->frames;
    if (f == nullptr || (f->nkids == 0 && !f->rescan) || v->doc == nullptr) {
        return false;
    }
    for (uint32_t i = f->nkids; i-- > 0;) {
        if (f->kids[i]->frames->el == nullptr) {
            kid_remove(v, i);
        }
    }
    embi_input_frame_focus_sync(v); /* a script moved the focus off a frame's iframe: the frame loses the keyboard */
    if (f->rescan) {
        f->rescan = false;
        rescan(v);
    }
    emb_view *root = embi_frames_root(v);
    bool any = false;
    for (uint32_t i = 0; i < f->nkids; i++) {
        emb_view *kid = f->kids[i];
        embi_frames *kf = kid->frames;
        if (kid->gfx == nullptr && root->gfx != nullptr) {
            embi_view_child_gfx(kid);
        }
        float box[4], sx, sy, cw, ch;
        if (kf->el != nullptr && content_box(v, kf->el, box, &sx, &sy, &cw, &ch)) {
            uint32_t w = (uint32_t)SDL_lroundf(cw), h = (uint32_t)SDL_lroundf(ch);
            w = w > 0 ? w : 1;
            h = h > 0 ? h : 1;
            if (w != kf->vw || h != kf->vh || kid->scale != root->scale || kid->scheme != root->scheme) {
                /* the size, or the root's scale or colour scheme (emb_view_set_scale, a DPI change, the system theme) */
                bool resized = kf->vw != 0 && kf->loaded && (w != kf->vw || h != kf->vh);
                kf->vw = w;
                kf->vh = h;
                embi_view_set_child_size(kid, w, h);
                if (resized) {
                    fire_resize(kid);
                }
            }
        }
        if (kf->load_pending) {
            kf->loaded = true;
            kid_load(v, kid);
        }
        if (kf->changed) {
            /* The picture of the last update was never drawn: its damage is gone, so the next draw is whole. */
            kid->force_full = true;
            kid->paint_dirty = true;
        }
        embi_script *ks = kid->doc != nullptr ? embi_doc_script(kid->doc) : nullptr;
        if (ks != nullptr) {
            embi_script_set_wake(ks, embi_frames_root(v)->mode == EMB_VIEW_WINDOWED); /* a task from a thread wakes the window */
        }
        if (embi_view_update(kid)) {
            kf->changed = true;
            any = true;
        }
    }
    return any;
}

typedef struct dmg_match {
    embi_frames *f;
} dmg_match;

/* Whether url names a frame of the list's view whose picture changed. */
static bool frame_match(void *user, embi_str url)
{
    dmg_match *m = user;
    if (url.len <= 12 || memcmp(url.ptr, "ember-frame:", 12) != 0) {
        return false;
    }
    char name[64];
    size_t n = url.len - 12 < sizeof name - 1 ? url.len - 12 : sizeof name - 1;
    memcpy(name, url.ptr + 12, n);
    name[n] = '\0';
    void *key = nullptr;
    (void)SDL_sscanf(name, "%p", &key);
    for (uint32_t i = 0; i < m->f->nkids; i++) {
        embi_frames *kf = m->f->kids[i]->frames;
        if (kf->changed && kf->el == key) {
            return true;
        }
    }
    return false;
}

void embi_frames_add_damage(emb_view *v, embi_damage *d, const embi_dl *cur)
{
    embi_frames *f = v->frames;
    if (f == nullptr || f->nkids == 0) {
        return;
    }
    bool any = false;
    for (uint32_t i = 0; i < f->nkids && !any; i++) {
        any = f->kids[i]->frames->changed;
    }
    if (any) {
        dmg_match m = {f};
        embi_damage_add_images(d, cur, v->scale, v->dev_w, v->dev_h, frame_match, &m);
    }
}

void embi_frames_draw(emb_view *v, SDL_GPUCommandBuffer *cmd)
{
    embi_frames *f = v->frames;
    if (f == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < f->nkids; i++) {
        emb_view *kid = f->kids[i];
        embi_frames_draw(kid, cmd);
        if (kid->frames->changed) {
            kid->frames->changed = false;
            (void)embi_view_draw(kid, cmd, true);
        }
    }
}

SDL_GPUTexture *embi_frames_texture(void *user, const void *key, uint32_t *w, uint32_t *h)
{
    emb_view *v = user;
    embi_frames *f = v->frames;
    if (f == nullptr) {
        return nullptr;
    }
    for (uint32_t i = 0; i < f->nkids; i++) {
        emb_view *kid = f->kids[i];
        if (kid->frames->el == key && kid->tex != nullptr) {
            *w = kid->dev_w;
            *h = kid->dev_h;
            return kid->tex;
        }
        SDL_GPUTexture *t = embi_frames_texture(kid, key, w, h);
        if (t != nullptr) {
            return t;
        }
    }
    return nullptr;
}

bool embi_frames_animating(emb_view *v)
{
    embi_frames *f = v->frames;
    for (uint32_t i = 0; f != nullptr && i < f->nkids; i++) {
        if (f->kids[i]->frames->load_pending || f->kids[i]->frames->el == nullptr || embi_view_animating(f->kids[i])) {
            return true;
        }
    }
    return f != nullptr && f->rescan;
}

bool embi_frames_next_due(emb_view *v, double *due_ms)
{
    embi_frames *f = v->frames;
    bool any = false;
    for (uint32_t i = 0; f != nullptr && i < f->nkids; i++) {
        emb_view *kid = f->kids[i];
        double d = 0;
        embi_script *s = kid->doc != nullptr ? embi_doc_script(kid->doc) : nullptr;
        if (s != nullptr && embi_script_next_due(s, &d) && (!any || d < *due_ms)) {
            *due_ms = d;
            any = true;
        }
        if (embi_frames_next_due(kid, &d) && (!any || d < *due_ms)) {
            *due_ms = d;
            any = true;
        }
    }
    return any;
}

bool embi_frames_has_tasks(emb_view *v)
{
    embi_frames *f = v->frames;
    for (uint32_t i = 0; f != nullptr && i < f->nkids; i++) {
        emb_view *kid = f->kids[i];
        embi_script *s = kid->doc != nullptr ? embi_doc_script(kid->doc) : nullptr;
        if ((s != nullptr && embi_script_has_tasks(s)) || embi_frames_has_tasks(kid)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ the bindings' hooks */

static uint32_t h_self(void *user)
{
    emb_view *v = user;
    return v->frames != nullptr ? v->frames->win_id : 0;
}

static uint32_t h_parent(void *user)
{
    emb_view *p = embi_frames_parent(user);
    return p != nullptr && p->frames != nullptr ? p->frames->win_id : 0;
}

static uint32_t h_top(void *user)
{
    emb_view *v = user;
    if (embi_frames_parent(v) == nullptr) {
        return 0;
    }
    emb_view *r = embi_frames_root(v);
    return r->frames != nullptr ? r->frames->win_id : 0;
}

static uint32_t h_content(void *user, embi_element *iframe)
{
    emb_view *v = user;
    emb_view *kid = kid_of(v->frames, iframe);
    embi_document *dom = v->doc != nullptr ? embi_doc_dom(v->doc) : nullptr;
    if (kid == nullptr && dom != nullptr && is_iframe(&iframe->base) && (iframe->base.flags & EMBI_NF_CONNECTED) != 0 &&
        iframe->base.doc == dom) {
        /* An iframe just inserted: its frame (with its initial empty document) is made now, as a browser does, so that
         * contentWindow works before the next update loads the frame. */
        kid = kid_new(v, iframe);
    }
    return kid != nullptr && kid->frames != nullptr ? kid->frames->win_id : 0;
}

static bool h_alive(void *user, uint32_t id)
{
    return find_win(embi_frames_root(user), id) != nullptr;
}

/* A message on its way to a window: posted to the receiver's loop. */
typedef struct msg_task {
    emb_view *to;
    uint32_t target, source;
    embi_sb bytes, origin, target_origin;
} msg_task;

static void msg_free(void *user)
{
    msg_task *t = user;
    embi_sb_free(&t->bytes);
    embi_sb_free(&t->origin);
    embi_sb_free(&t->target_origin);
    SDL_free(t);
}

/* The origin of v's document, serialized ("null" for an opaque one). */
static void view_origin(emb_view *v, embi_sb *out)
{
    embi_url u;
    memset(&u, 0, sizeof u);
    if (v->doc != nullptr && embi_url_parse(embi_doc_url(v->doc), nullptr, &u)) {
        embi_url_origin(&u, out);
        embi_url_free(&u);
    } else {
        embi_sb_append(out, EMBI_STR("null"));
    }
}

static void msg_run(void *user)
{
    msg_task *t = user;
    emb_view *v = t->to;
    if (v->frames == nullptr || v->frames->win_id != t->target || !embi_frames_live(v) || v->doc == nullptr) {
        msg_free(t); /* the window navigated or is going away */
        return;
    }
    if (t->target_origin.len > 0) {
        embi_sb o = {nullptr, 0, 0};
        view_origin(v, &o);
        bool match = o.len == t->target_origin.len && memcmp(o.data, t->target_origin.data, o.len) == 0;
        embi_sb_free(&o);
        if (!match) {
            msg_free(t);
            return;
        }
    }
    embi_script *s = embi_doc_script(v->doc);
    embi_bind_ctx *c = s != nullptr ? embi_script_bind_if(s) : nullptr;
    if (c != nullptr) {
        embi_bind_frames_deliver(c, (embi_str){t->bytes.data, (uint32_t)t->bytes.len},
                                 (embi_str){t->origin.data, (uint32_t)t->origin.len}, t->source);
        embi_js_run_jobs(c->rt);
    }
    msg_free(t);
}

static void h_post(void *user, uint32_t target, uint32_t source, embi_sb *bytes, embi_str origin, embi_str target_origin)
{
    emb_view *to = find_win(embi_frames_root(user), target);
    embi_script *s = to != nullptr && to->doc != nullptr ? embi_doc_script(to->doc) : nullptr;
    msg_task *t = s != nullptr ? SDL_calloc(1, sizeof *t) : nullptr;
    if (t == nullptr) {
        embi_sb_free(bytes);
        return;
    }
    t->to = to;
    t->target = target;
    t->source = source;
    t->bytes = *bytes;
    *bytes = (embi_sb){nullptr, 0, 0};
    embi_sb_append(&t->origin, origin);
    embi_sb_append(&t->target_origin, target_origin);
    if (!embi_script_post_ex(s, msg_run, t, msg_free)) {
        msg_free(t);
    }
}

void embi_frames_bind(embi_bind_ctx *c, emb_view *v)
{
    if (c == nullptr || v == nullptr || fr_of(v) == nullptr) {
        return;
    }
    embi_bind_frames h = {
        .user = v,
        .self_id = h_self,
        .parent_id = h_parent,
        .top_id = h_top,
        .content_id = h_content,
        .alive = h_alive,
        .post = h_post,
    };
    embi_bind_set_frames(c, &h);
    /* The realms of one view's frames share one MessagePort registry, so a port transferred to another frame keeps its
     * peer (every frame runs on the view's thread). */
    embi_frames *top = fr_of(embi_frames_root(v));
    if (top != nullptr) {
        if (top->ports == nullptr) {
            top->ports = embi_mports_new();
        }
        embi_loader_share_ports(c, top->ports);
    }
}
