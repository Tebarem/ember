#include "image/image.h"

#include "image/internal.h"


/* The frames of an animated GIF and where the frame clock stands in them. */
typedef struct embi_image_anim {
    embi_gif_frames f;
    double total_ms; /* one play */
    uint32_t cur;    /* the frame shown */
    uint32_t plays;  /* plays completed */
    double start_ms; /* clock time the current frame began */
    bool started;    /* the first tick has set start_ms */
    bool done;       /* a finite loop count ran out: the last frame stays */
} embi_image_anim;

typedef struct embi_image_entry {
    char *url; /* copy, nul-terminated */
    uint32_t url_len;
    uint64_t hash;
    uint8_t *enc; /* encoded bytes, kept for re-decoding after eviction */
    size_t enc_len;
    embi_image_state state;
    uint32_t w;
    uint32_t h;
    uint8_t *px; /* straight RGBA8, or nullptr when not decoded or evicted */
    uint64_t last_use;
    bool has_handle;
    uint64_t handle; /* GPU handle from the hook, valid while has_handle */
    embi_image_anim *an; /* animated GIF: all frames (px is a copy of the current one while it is held) */
    double vis_ms;       /* clock time (the cache's last tick) of the last upload or pixel read: on screen lately */
    bool anim_dropped;   /* trim dropped the frames of an animated GIF: a draw decodes them again */
} embi_image_entry;

struct embi_image_cache {
    size_t budget;
    size_t px_bytes;
    uint64_t max_pixels;
    uint64_t tick;
    uint64_t trim_tick; /* tick at the previous trim: entries used after it are recent */
    size_t enc_bytes;   /* encoded bytes held by all entries (not budgeted) */
    embi_image_decoder_fn dec;
    void *dec_user;
    bool apply_orientation;
    embi_image_gpu gpu;
    embi_image_entry **ents;
    uint32_t nents;
    uint32_t cap_ents;
    uint32_t *slots; /* entry index + 1, 0 = empty; power-of-two length */
    uint32_t nslots;
    size_t anim_bytes; /* frames of animated GIFs (capped separately from the pixel budget) */
    size_t anim_cap_image; /* most frame bytes one image may keep, else it stays a still */
    size_t anim_cap_total; /* most frame bytes all images keep together */
    double now_ms;     /* the clock time of the last embi_image_cache_tick */
};

/* Frame memory of animated GIFs: one image over the first cap, or images over the second together, stay stills (the
 * first frame). Frames of an image not drawn lately are dropped by trim. */
#define EMBI_IMAGE_ANIM_MAX_BYTES ((size_t)8 << 20)
#define EMBI_IMAGE_ANIM_TOTAL_BYTES ((size_t)16 << 20)

static size_t px_size(uint32_t w, uint32_t h)
{
    return (size_t)w * h * 4;
}

void embi_pixels_free(embi_pixels *p)
{
    if (p->px) {
        embi_free(p->px, px_size(p->w, p->h));
    }
    p->px = nullptr;
    p->w = 0;
    p->h = 0;
}

/* ------------------------------------------------------------------ entries and the URL table */

static void entry_release_handle(embi_image_cache *c, embi_image_entry *e)
{
    if (e->has_handle) {
        if (c->gpu.release) {
            c->gpu.release(c->gpu.user, e->handle);
        }
        e->has_handle = false;
        e->handle = 0;
    }
}

static void entry_drop_pixels(embi_image_cache *c, embi_image_entry *e)
{
    entry_release_handle(c, e);
    if (e->px) {
        c->px_bytes -= px_size(e->w, e->h);
        embi_free(e->px, px_size(e->w, e->h));
        e->px = nullptr;
    }
}

static void entry_drop_encoded(embi_image_cache *c, embi_image_entry *e)
{
    if (e->enc) {
        embi_free(e->enc, e->enc_len ? e->enc_len : 1);
        c->enc_bytes -= e->enc_len;
        e->enc = nullptr;
        e->enc_len = 0;
    }
}

static void entry_drop_anim(embi_image_cache *c, embi_image_entry *e)
{
    if (e->an) {
        c->anim_bytes -= (size_t)e->an->f.n * e->an->f.w * e->an->f.h * 4;
        embi_gif_frames_free(&e->an->f);
        embi_free(e->an, sizeof *e->an);
        e->an = nullptr;
    }
}

static void entry_drop_all(embi_image_cache *c, embi_image_entry *e)
{
    e->anim_dropped = false;
    entry_drop_pixels(c, e);
    entry_drop_anim(c, e);
    entry_drop_encoded(c, e);
    e->w = 0;
    e->h = 0;
}

static void slot_place(embi_image_cache *c, uint32_t index)
{
    uint32_t mask = c->nslots - 1;
    uint32_t i = (uint32_t)c->ents[index]->hash & mask;
    while (c->slots[i] != 0) {
        i = (i + 1) & mask;
    }
    c->slots[i] = index + 1;
}

static void grow_slots(embi_image_cache *c)
{
    uint32_t ns = c->nslots ? c->nslots * 2 : 16;
    if (c->slots) {
        embi_free(c->slots, (size_t)c->nslots * sizeof *c->slots);
    }
    c->slots = embi_calloc(ns, sizeof *c->slots);
    c->nslots = ns;
    for (uint32_t i = 0; i < c->nents; i++) {
        slot_place(c, i);
    }
}

static embi_image_entry *find_entry(const embi_image_cache *c, embi_str url, uint64_t hash)
{
    if (c->nslots == 0) {
        return nullptr;
    }
    uint32_t mask = c->nslots - 1;
    for (uint32_t i = (uint32_t)hash & mask;; i = (i + 1) & mask) {
        uint32_t s = c->slots[i];
        if (s == 0) {
            return nullptr;
        }
        embi_image_entry *e = c->ents[s - 1];
        if (e->hash == hash && e->url_len == url.len &&
            SDL_memcmp(e->url, url.ptr, url.len) == 0) {
            return e;
        }
    }
}

static embi_image_entry *create_entry(embi_image_cache *c, embi_str url, uint64_t hash)
{
    if ((c->nents + 1u) * 2u > c->nslots) {
        grow_slots(c);
    }
    if (c->nents == c->cap_ents) {
        uint32_t nc = c->cap_ents ? c->cap_ents * 2 : 16;
        c->ents = embi_realloc(c->ents, (size_t)c->cap_ents * sizeof *c->ents,
                               (size_t)nc * sizeof *c->ents);
        c->cap_ents = nc;
    }
    embi_image_entry *e = embi_calloc(1, sizeof *e);
    e->url = embi_alloc((size_t)url.len + 1);
    SDL_memcpy(e->url, url.ptr, url.len);
    e->url[url.len] = '\0';
    e->url_len = url.len;
    e->hash = hash;
    e->state = EMBI_IMAGE_NONE;
    c->ents[c->nents] = e;
    c->nents++;
    slot_place(c, c->nents - 1);
    return e;
}

static embi_image_entry *get_or_create(embi_image_cache *c, embi_str url)
{
    uint64_t hash = embi_hash_bytes(url.ptr, url.len, 0);
    embi_image_entry *e = find_entry(c, url, hash);
    if (!e) {
        e = create_entry(c, url, hash);
    }
    return e;
}

static embi_image_entry *lookup(const embi_image_cache *c, embi_str url)
{
    return find_entry(c, url, embi_hash_bytes(url.ptr, url.len, 0));
}

/* ------------------------------------------------------------------ decoding */

/* A format some decoder knows: PNG, JPEG and GIF always, anything else only with an embedder
 * callback. */
static bool format_known(const embi_image_cache *c, const uint8_t *in, size_t n)
{
    return embi_image_sniff(in, n) != EMBI_IMAGE_FMT_OTHER || c->dec != nullptr;
}

/* Decodes encoded bytes to straight RGBA8 (see embi_image_decode for the dispatch). */
static bool decode_bytes(const embi_image_cache *c, const uint8_t *in, size_t n, embi_pixels *out)
{
    return embi_image_decode(in, n, c->max_pixels, c->apply_orientation, c->dec, c->dec_user, out);
}

/* Makes sure a READY entry has its pixels. Returns false if they cannot be decoded again. */
static bool ensure_pixels(embi_image_cache *c, embi_image_entry *e)
{
    if (e->px) {
        return true;
    }
    if (e->an) {
        size_t fb = px_size(e->w, e->h);
        e->px = embi_alloc(fb);
        SDL_memcpy(e->px, e->an->f.px + (size_t)e->an->cur * fb, fb);
        c->px_bytes += fb;
        return true;
    }
    if (!e->enc) {
        return false;
    }
    embi_pixels p = {0};
    if (!decode_bytes(c, e->enc, e->enc_len, &p) || p.w != e->w || p.h != e->h) {
        embi_pixels_free(&p);
        entry_drop_all(c, e);
        e->state = EMBI_IMAGE_FAILED;
        return false;
    }
    e->px = p.px;
    c->px_bytes += px_size(e->w, e->h);
    return true;
}

/* ------------------------------------------------------------------ public API */

embi_image_cache *embi_image_cache_create(size_t budget_bytes)
{
    embi_image_cache *c = embi_calloc(1, sizeof *c);
    c->budget = budget_bytes;
    c->max_pixels = EMBI_IMAGE_DEFAULT_MAX_PIXELS;
    c->anim_cap_image = EMBI_IMAGE_ANIM_MAX_BYTES;
    c->anim_cap_total = EMBI_IMAGE_ANIM_TOTAL_BYTES;
    c->apply_orientation = true;
    return c;
}

void embi_image_cache_destroy(embi_image_cache *c)
{
    for (uint32_t i = 0; i < c->nents; i++) {
        embi_image_entry *e = c->ents[i];
        entry_drop_all(c, e);
        embi_free(e->url, (size_t)e->url_len + 1);
        embi_free(e, sizeof *e);
    }
    if (c->ents) {
        embi_free(c->ents, (size_t)c->cap_ents * sizeof *c->ents);
    }
    if (c->slots) {
        embi_free(c->slots, (size_t)c->nslots * sizeof *c->slots);
    }
    embi_free(c, sizeof *c);
}

void embi_image_cache_set_budget(embi_image_cache *c, size_t bytes)
{
    c->budget = bytes;
}

void embi_image_cache_set_max_pixels(embi_image_cache *c, uint64_t pixels)
{
    c->max_pixels = pixels;
}

void embi_image_cache_set_decoder(embi_image_cache *c, embi_image_decoder_fn fn, void *user)
{
    c->dec = fn;
    c->dec_user = user;
}

void embi_image_cache_set_apply_orientation(embi_image_cache *c, bool on)
{
    c->apply_orientation = on;
}

void embi_image_cache_set_gpu(embi_image_cache *c, const embi_image_gpu *gpu)
{
    for (uint32_t i = 0; i < c->nents; i++) {
        entry_release_handle(c, c->ents[i]);
    }
    c->gpu = gpu ? *gpu : (embi_image_gpu){0};
}

void embi_image_cache_begin(embi_image_cache *c, embi_str url)
{
    embi_image_entry *e = get_or_create(c, url);
    if (e->state == EMBI_IMAGE_NONE) {
        e->state = EMBI_IMAGE_PENDING;
    }
}

/* Decodes the frames of a GIF entry within the caps; the entry stays a still when they do not fit. */
static void entry_attach_anim(embi_image_cache *c, embi_image_entry *e)
{
    embi_gif_frames f;
    size_t room = c->anim_cap_total > c->anim_bytes ? c->anim_cap_total - c->anim_bytes : 0;
    size_t lim = room < c->anim_cap_image ? room : c->anim_cap_image;
    if (lim > 0 && embi_gif_decode_frames(e->enc, e->enc_len, c->max_pixels, lim, &f)) {
        if (f.w == e->w && f.h == e->h) {
            e->an = embi_calloc(1, sizeof *e->an);
            e->an->f = f;
            for (uint32_t i = 0; i < f.n; i++) {
                e->an->total_ms += f.delay_ms[i];
            }
            c->anim_bytes += (size_t)f.n * f.w * f.h * 4;
        } else {
            embi_gif_frames_free(&f);
        }
    }
}

/* A draw of a GIF whose frames trim dropped decodes them again (the clock restarts at frame 0). */
static void entry_restore_anim(embi_image_cache *c, embi_image_entry *e)
{
    if (!e->anim_dropped || e->an || !e->enc) {
        return;
    }
    e->anim_dropped = false;
    entry_attach_anim(c, e);
    if (e->an && e->px) {
        size_t fb = px_size(e->w, e->h);
        SDL_memcpy(e->px, e->an->f.px, fb);
        entry_release_handle(c, e);
    }
}

embi_image_state embi_image_cache_finish(embi_image_cache *c, embi_str url, const uint8_t *bytes,
                                         size_t len)
{
    embi_image_entry *e = get_or_create(c, url);
    entry_drop_all(c, e);
    e->enc = embi_alloc(len ? len : 1);
    if (len) {
        SDL_memcpy(e->enc, bytes, len);
    }
    e->enc_len = len;
    c->enc_bytes += len;

    embi_pixels p = {0};
    if (decode_bytes(c, bytes, len, &p)) {
        e->w = p.w;
        e->h = p.h;
        e->px = p.px;
        c->px_bytes += px_size(p.w, p.h);
        e->state = EMBI_IMAGE_READY;
        e->last_use = ++c->tick;
        e->vis_ms = c->now_ms;
        if (embi_image_sniff(bytes, len) == EMBI_IMAGE_FMT_GIF) {
            entry_attach_anim(c, e);
        }
    } else {
        e->state = format_known(c, bytes, len) ? EMBI_IMAGE_FAILED : EMBI_IMAGE_UNSUPPORTED;
        entry_drop_encoded(c, e);
    }
    return e->state;
}

void embi_image_cache_fail(embi_image_cache *c, embi_str url)
{
    embi_image_entry *e = get_or_create(c, url);
    entry_drop_all(c, e);
    e->state = EMBI_IMAGE_FAILED;
}

void embi_image_cache_forget(embi_image_cache *c, embi_str url)
{
    embi_image_entry *e = lookup(c, url);
    if (e) {
        entry_drop_all(c, e);
        e->state = EMBI_IMAGE_NONE;
    }
}

embi_image_state embi_image_cache_query(const embi_image_cache *c, embi_str url, uint32_t *w,
                                        uint32_t *h)
{
    const embi_image_entry *e = lookup(c, url);
    embi_image_state st = e ? e->state : EMBI_IMAGE_NONE;
    if (w) {
        *w = st == EMBI_IMAGE_READY ? e->w : 0;
    }
    if (h) {
        *h = st == EMBI_IMAGE_READY ? e->h : 0;
    }
    return st;
}

bool embi_image_cache_pixels(embi_image_cache *c, embi_str url, const uint8_t **px, uint32_t *w,
                             uint32_t *h)
{
    embi_image_entry *e = lookup(c, url);
    if (e && e->state == EMBI_IMAGE_READY) {
        entry_restore_anim(c, e);
    }
    if (!e || e->state != EMBI_IMAGE_READY || !ensure_pixels(c, e)) {
        return false;
    }
    e->last_use = ++c->tick;
    e->vis_ms = c->now_ms;
    *px = e->px;
    *w = e->w;
    *h = e->h;
    return true;
}

bool embi_image_cache_upload(embi_image_cache *c, embi_str url, SDL_GPUCommandBuffer *cmd,
                             uint64_t *handle)
{
    embi_image_entry *e = lookup(c, url);
    if (!c->gpu.upload || !e || e->state != EMBI_IMAGE_READY) {
        return false;
    }
    entry_restore_anim(c, e);
    if (!e->has_handle) {
        if (!ensure_pixels(c, e)) {
            return false;
        }
        uint64_t h = c->gpu.upload(c->gpu.user, e->px, e->w, e->h, cmd);
        if (h == 0) {
            return false;
        }
        e->handle = h;
        e->has_handle = true;
    }
    e->last_use = ++c->tick;
    e->vis_ms = c->now_ms;
    *handle = e->handle;
    return true;
}

static int cmp_last_use(const void *a, const void *b)
{
    const embi_image_entry *x = *(embi_image_entry *const *)a;
    const embi_image_entry *y = *(embi_image_entry *const *)b;
    return x->last_use < y->last_use ? -1 : x->last_use > y->last_use;
}

void embi_image_cache_trim(embi_image_cache *c)
{
    uint64_t recent_after = c->trim_tick;
    c->trim_tick = c->tick;
    /* Frames of animated images that were not drawn lately go; the still stays. */
    for (uint32_t i = 0; i < c->nents; i++) {
        embi_image_entry *e = c->ents[i];
        if (e->an && c->now_ms - e->vis_ms > (double)e->an->f.delay_ms[e->an->cur] + 1000.0) {
            entry_drop_anim(c, e);
            e->anim_dropped = true;
        }
    }
    if (c->px_bytes <= c->budget || c->nents == 0) {
        return;
    }
    /* One sort per trim, then drop from the oldest. An entry larger than the budget is not a
     * candidate while it was used after the previous trim, so it is not decoded again each frame. */
    embi_image_entry **cand = embi_alloc((size_t)c->nents * sizeof *cand);
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->nents; i++) {
        embi_image_entry *e = c->ents[i];
        bool oversize = e->px && px_size(e->w, e->h) > c->budget;
        if (e->px && !(oversize && e->last_use > recent_after)) {
            cand[n++] = e;
        }
    }
    qsort(cand, n, sizeof *cand, cmp_last_use);
    for (uint32_t k = 0; k < n && c->px_bytes > c->budget; k++) {
        entry_drop_pixels(c, cand[k]);
    }
    embi_free(cand, (size_t)c->nents * sizeof *cand);
}

size_t embi_image_cache_bytes(const embi_image_cache *c)
{
    return c->px_bytes;
}

size_t embi_image_cache_encoded_bytes(const embi_image_cache *c)
{
    return c->enc_bytes;
}

/* ------------------------------------------------------------------ animated GIF */

uint32_t embi_image_cache_frame_count(const embi_image_cache *c, embi_str url)
{
    const embi_image_entry *e = lookup(c, url);
    if (!e || e->state != EMBI_IMAGE_READY) {
        return 0;
    }
    return e->an ? e->an->f.n : 1;
}

uint32_t embi_image_cache_frame(const embi_image_cache *c, embi_str url)
{
    const embi_image_entry *e = lookup(c, url);
    return e && e->an ? e->an->cur : 0;
}

/* Moves the frame to the clock. Returns true when the frame shown changed. */
static bool anim_advance(embi_image_cache *c, embi_image_entry *e, double now)
{
    embi_image_anim *a = e->an;
    if (a->done) {
        return false;
    }
    if (!a->started) {
        a->started = true;
        a->start_ms = now;
        return false;
    }
    double el = now - a->start_ms;
    if (!(el >= (double)a->f.delay_ms[a->cur])) {
        return false;
    }
    uint32_t old = a->cur;
    bool skipped_to_end = false;
    if (el >= a->total_ms) {
        /* Whole plays at once, so a long pause is not walked frame by frame. */
        double cycles = SDL_floor(el / a->total_ms);
        if (a->f.loops != 0 && cycles >= (double)(a->f.loops - a->plays)) {
            a->plays = a->f.loops;
            a->cur = a->f.n - 1;
            a->done = true;
            skipped_to_end = true;
        } else {
            a->plays += a->f.loops != 0 ? (uint32_t)cycles : 0;
            a->start_ms += cycles * a->total_ms;
            el -= cycles * a->total_ms;
        }
    }
    while (!skipped_to_end && el >= (double)a->f.delay_ms[a->cur]) {
        el -= (double)a->f.delay_ms[a->cur];
        a->start_ms += (double)a->f.delay_ms[a->cur];
        if (a->cur + 1 == a->f.n) {
            a->plays++;
            if (a->f.loops != 0 && a->plays >= a->f.loops) {
                a->done = true;
                break;
            }
            a->cur = 0;
        } else {
            a->cur++;
        }
    }
    if (a->cur == old) {
        return false;
    }
    /* The picture changes: the GPU copy of the old frame goes, the next draw uploads this one. */
    entry_release_handle(c, e);
    if (e->px) {
        size_t fb = px_size(e->w, e->h);
        SDL_memcpy(e->px, a->f.px + (size_t)a->cur * fb, fb);
    }
    return true;
}

uint32_t embi_image_cache_tick(embi_image_cache *c, double now_ms, embi_image_frame_fn cb, void *user)
{
    c->now_ms = now_ms;
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->nents; i++) {
        embi_image_entry *e = c->ents[i];
        if (e->an && e->state == EMBI_IMAGE_READY && anim_advance(c, e, now_ms)) {
            n++;
            if (cb) {
                cb(user, (embi_str){e->url, e->url_len});
            }
        }
    }
    return n;
}

bool embi_image_cache_animating(const embi_image_cache *c, double now_ms)
{
    for (uint32_t i = 0; i < c->nents; i++) {
        const embi_image_entry *e = c->ents[i];
        if (e->an && e->state == EMBI_IMAGE_READY && !e->an->done &&
            now_ms - e->vis_ms <= (double)e->an->f.delay_ms[e->an->cur] + 1000.0) {
            return true;
        }
    }
    return false;
}

size_t embi_image_cache_anim_bytes(const embi_image_cache *c)
{
    return c->anim_bytes;
}

void embi_image_cache_set_anim_caps(embi_image_cache *c, size_t per_image, size_t total)
{
    c->anim_cap_image = per_image;
    c->anim_cap_total = total;
}
