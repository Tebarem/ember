#include "embed/svgcache.h"

#if EMBER_SVG

#include "svg/svg.h"

/* A parsed document costs about ten times its source in DOM and styles, so the cap is on the source bytes: 512 KB keeps the
 * parsed images of a view in a few MB. The least recently used images are dropped (they are fetched and parsed again when
 * asked for), so a page with hundreds of distinct icons does not keep them all.
 *
 * Images are dropped only by embi_svgcache_trim, which the view calls where no paint is running (after it draws a frame,
 * through embi_gfx_trim). A paint holds the images it got until it ends: an image being painted can ask for nested
 * SVG images, which are added, and adding never frees anything. */
#define SVGCACHE_MAX_BYTES (512u * 1024u)
#define SVGCACHE_MAX_ENTRIES 256u

typedef struct ent {
    char *url;
    uint32_t len;
    embi_svg_image *img;
    uint64_t stamp; /* the last use */
    size_t bytes;
} ent;

struct embi_svgcache {
    EMBI_VEC(ent) v;
    uint64_t tick;
    size_t bytes;
};

embi_svgcache *embi_svgcache_create(void)
{
    return embi_calloc(1, sizeof(embi_svgcache));
}

void embi_svgcache_destroy(embi_svgcache *c)
{
    if (c == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < c->v.len; i++) {
        embi_svg_image_free(c->v.data[i].img);
        embi_free(c->v.data[i].url, (size_t)c->v.data[i].len + 1);
    }
    embi_vec_free(&c->v);
    embi_free(c, sizeof *c);
}

static ent *find(const embi_svgcache *c, embi_str url)
{
    for (uint32_t i = c->v.len; i-- > 0;) { /* the recent ones are at the end */
        ent *e = &c->v.data[i];
        if (e->len == url.len && (url.len == 0 || memcmp(e->url, url.ptr, url.len) == 0)) {
            return e;
        }
    }
    return nullptr;
}

/* Drops the least recently used images until the cache fits, keeping the newest. */
void embi_svgcache_trim(embi_svgcache *c)
{
    while (c != nullptr && c->v.len > 1 && (c->bytes > SVGCACHE_MAX_BYTES || c->v.len > SVGCACHE_MAX_ENTRIES)) {
        int64_t old = -1;
        for (uint32_t i = 0; i + 1 < c->v.len; i++) { /* the newest one stays */
            if (old < 0 || c->v.data[i].stamp < c->v.data[old].stamp) {
                old = i;
            }
        }
        if (old < 0) {
            return;
        }
        ent *e = &c->v.data[old];
        c->bytes -= e->bytes;
        embi_svg_image_free(e->img);
        embi_free(e->url, (size_t)e->len + 1);
        memmove(e, e + 1, (size_t)((int64_t)c->v.len - old - 1) * sizeof *e);
        c->v.len--;
    }
}

const embi_svg_image *embi_svgcache_get(const embi_svgcache *c, embi_str url)
{
    ent *e = c != nullptr ? find(c, url) : nullptr;
    if (e == nullptr) {
        return nullptr;
    }
    e->stamp = ++((embi_svgcache *)c)->tick;
    return e->img;
}

bool embi_svgcache_add(embi_svgcache *c, embi_style_engine *se, embi_str url, const uint8_t *bytes, size_t len)
{
    if (c == nullptr || se == nullptr || bytes == nullptr || !embi_svg_sniff(bytes, len)) {
        return false;
    }
    ent *old = find(c, url);
    if (old != nullptr) {
        old->stamp = ++c->tick;
        return true;
    }
    embi_svg_image *img = embi_svg_image_load(se, bytes, len);
    if (img == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < url.len; i++) {
        if (url.ptr[i] == '#') {
            embi_svg_image_set_fragment(img, (embi_str){url.ptr + i + 1, url.len - i - 1});
            break;
        }
    }
    ent e;
    e.len = url.len;
    e.url = embi_alloc((size_t)url.len + 1);
    if (url.len > 0) {
        memcpy(e.url, url.ptr, url.len);
    }
    e.url[url.len] = '\0';
    e.img = img;
    e.bytes = embi_svg_image_bytes(img);
    e.stamp = ++c->tick;
    c->bytes += e.bytes;
    embi_vec_push(&c->v, e);
    return true;
}

bool embi_svgcache_size(const embi_svgcache *c, embi_str url, float *w, float *h, bool *has_ratio)
{
    const embi_svg_image *img = embi_svgcache_get(c, url);
    if (img == nullptr) {
        return false;
    }
    embi_svg_image_size(img, w, h, has_ratio);
    return true;
}

size_t embi_svgcache_bytes(const embi_svgcache *c)
{
    return c->bytes;
}
#else
typedef int embi_svg_unit_empty; /* EMBER_SVG=OFF: nothing here (SZ3b) */
#endif
