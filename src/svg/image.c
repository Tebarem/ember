/*
 * SVG documents as images (R4c): <img src=x.svg>, CSS url(x.svg) in backgrounds, border-image and list-style-image, and data:
 * URLs. The document is parsed apart from the page (a DOM of its own, scripts off, no loads), styled with the engine, and
 * painted as vector items at the size it is drawn, so it stays sharp at any scale.
 */
#include "svg/svg_int.h"

#include "html/parser.h"
#include "paint/dl.h"

#include <SDL3/SDL_stdinc.h>

struct embi_svg_image {
    embi_style_engine *se;
    embi_document *doc;
    embi_element *root;
    float w, h;
    bool ratio;
    bool has_vb; /* the root has a viewBox: it is fitted to the drawn size by preserveAspectRatio */
    size_t bytes;
    const embi_element *view; /* the <view> element the URL fragment names */
};

bool embi_svg_sniff(const uint8_t *b, size_t n)
{
    size_t i = 0;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        i = 3;
    }
    for (;;) {
        while (i < n && (b[i] == ' ' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r' || b[i] == '\f')) {
            i++;
        }
        if (i + 1 < n && b[i] == '<' && b[i + 1] == '?') {
            while (i + 1 < n && !(b[i] == '?' && b[i + 1] == '>')) {
                i++;
            }
            i += 2;
            continue;
        }
        if (i + 3 < n && memcmp(b + i, "<!--", 4) == 0) {
            i += 4;
            while (i + 2 < n && memcmp(b + i, "-->", 3) != 0) {
                i++;
            }
            i += 3;
            continue;
        }
        if (i + 1 < n && b[i] == '<' && b[i + 1] == '!') {
            int depth = 0;
            while (i < n) {
                if (b[i] == '[') {
                    depth++;
                } else if (b[i] == ']') {
                    depth--;
                } else if (b[i] == '>' && depth <= 0) {
                    break;
                }
                i++;
            }
            i++;
            continue;
        }
        break;
    }
    if (i + 4 > n || b[i] != '<') {
        return false;
    }
    i++;
    /* an optional prefix: <svg:svg ...> */
    size_t j = i;
    while (j < n && b[j] != ':' && b[j] != ' ' && b[j] != '>' && b[j] != '/' && b[j] != '\t' && b[j] != '\n') {
        j++;
    }
    if (j < n && b[j] == ':') {
        i = j + 1;
    }
    if (i + 3 > n || SDL_strncasecmp((const char *)b + i, "svg", 3) != 0) {
        return false;
    }
    return i + 3 == n || b[i + 3] == ' ' || b[i + 3] == '>' || b[i + 3] == '/' || b[i + 3] == '\t' || b[i + 3] == '\n' ||
           b[i + 3] == '\r';
}

static embi_element *find_svg_root(embi_document *doc)
{
    embi_node *root = &doc->base;
    for (embi_node *n = embi_next_in_tree(root, root); n != nullptr; n = embi_next_in_tree(root, n)) {
        if (embi_is_element(n) && svg_tag(embi_as_element(n)) == TG_SVG) {
            return embi_as_element(n);
        }
    }
    return nullptr;
}

static void child_text(const embi_node *n, embi_sb *out)
{
    for (const embi_node *c = n->first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_TEXT) {
            embi_chardata *cd = (embi_chardata *)(void *)c;
            embi_dstr_append_utf8(out, cd->data);
        }
    }
}

static void compute_size(embi_svg_image *img)
{
    float w = -1, h = -1;
    svg_len l;
    if (svg_attr_len_raw(img->root, "width", &l) && l.unit != SVGL_PCT) {
        w = svg_len_px(l, 16.0f, 0);
    }
    if (svg_attr_len_raw(img->root, "height", &l) && l.unit != SVGL_PCT) {
        h = svg_len_px(l, 16.0f, 0);
    }
    float vb[4], m[6];
    bool has_vb = false;
    svg_ctx c;
    svg_ctx_init(&c, nullptr, SM_HIT);
    svg_viewport_matrix(&c, img->root, 1, 1, m, vb, &has_vb);
    svg_ctx_free(&c);
    img->has_vb = has_vb;
    if (w >= 0 && h >= 0) {
        img->ratio = w > 0 && h > 0;
    } else if (has_vb) {
        float r = vb[2] / vb[3];
        if (w >= 0) {
            h = w / r;
        } else if (h >= 0) {
            w = h * r;
        } else {
            w = vb[2];
            h = vb[3];
        }
        img->ratio = true;
    } else {
        if (w < 0) {
            w = 300;
        }
        if (h < 0) {
            h = 150;
        }
        img->ratio = false;
    }
    img->w = w;
    img->h = h;
}

embi_svg_image *embi_svg_image_load(embi_style_engine *se, const uint8_t *bytes, size_t len)
{
    if (se == nullptr || bytes == nullptr || !embi_svg_sniff(bytes, len)) {
        return nullptr;
    }
    embi_document *doc = embi_document_new(true);
    embi_html_parser_config cfg = {.scripting = false};
    embi_html_parser *p = embi_html_parser_new(doc, &cfg);
    embi_html_parser_write(p, bytes, len);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    embi_element *root = find_svg_root(doc);
    if (root == nullptr) {
        embi_document_free(doc);
        return nullptr;
    }
    /* <style> elements of the document */
    embi_css_sheet *sheets[16];
    uint32_t ns = 0;
    embi_node *top = &doc->base;
    for (embi_node *n = embi_next_in_tree(top, top); n != nullptr && ns < 16; n = embi_next_in_tree(top, n)) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *el = embi_as_element(n);
        embi_str nm = embi_atom_str(el->base.name);
        if (nm.len == 5 && memcmp(nm.ptr, "style", 5) == 0) {
            embi_sb sb = {0};
            child_text(n, &sb);
            embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
            embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
            embi_css_sheet *sh = embi_css_sheet_parse((embi_str){sb.data != nullptr ? sb.data : "", sb.len}, &opts);
            embi_sb_free(&sb);
            if (sh != nullptr) {
                sheets[ns++] = sh;
            }
        }
    }
    /* even with no sheets: this attaches the document to the engine */
    embi_document_set_author_sheets(se, doc, sheets, ns);
    for (uint32_t i = 0; i < ns; i++) {
        embi_css_sheet_unref(sheets[i]);
    }
    embi_style_resolve_document(se, doc);
    embi_svg_image *img = embi_calloc(1, sizeof *img);
    img->se = se;
    img->doc = doc;
    img->root = root;
    img->bytes = len;
    compute_size(img);
    return img;
}

void embi_svg_image_free(embi_svg_image *img)
{
    if (img == nullptr) {
        return;
    }
    embi_style_forget_document(img->se, img->doc);
    embi_document_free(img->doc);
    embi_free(img, sizeof *img);
}

void embi_svg_image_size(const embi_svg_image *img, float *w, float *h, bool *has_ratio)
{
    *w = img->w;
    *h = img->h;
    if (has_ratio != nullptr) {
        *has_ratio = img->ratio;
    }
}

void embi_svg_image_set_fragment(embi_svg_image *img, embi_str frag)
{
    if (img == nullptr) {
        return;
    }
    img->view = nullptr;
    if (frag.len == 0) {
        return;
    }
    char tmp[130];
    if (frag.len > 128) {
        return;
    }
    tmp[0] = '#';
    memcpy(tmp + 1, frag.ptr, frag.len);
    embi_element *v = svg_ref_element(img->doc, (embi_str){tmp, frag.len + 1});
    if (v != nullptr && svg_tag(v) == TG_VIEW) {
        img->view = v;
    }
}

size_t embi_svg_image_bytes(const embi_svg_image *img)
{
    return img != nullptr ? img->bytes + sizeof *img : 0;
}

/* Inside an SVG image only data: URLs load, as in browsers: the host callbacks are reached through this filter. */
typedef struct data_fwd {
    const embi_svg_env *parent;
} data_fwd;

static bool is_data_url(embi_str u)
{
    return u.ptr != nullptr && u.len >= 5 && SDL_strncasecmp(u.ptr, "data:", 5) == 0;
}

static bool fwd_image_size(void *user, embi_str url, float *w, float *h, bool *has_ratio)
{
    const embi_svg_env *p = ((data_fwd *)user)->parent;
    return is_data_url(url) && p != nullptr && p->image_size != nullptr && p->image_size(p->user, url, w, h, has_ratio);
}

static const embi_svg_image *fwd_svg_image(void *user, embi_str url)
{
    const embi_svg_env *p = ((data_fwd *)user)->parent;
    return is_data_url(url) && p != nullptr && p->svg_image != nullptr ? p->svg_image(p->user, url) : nullptr;
}

void embi_svg_paint_image(const embi_svg_env *env, embi_dl *dl, const embi_svg_image *img, const float dest[4],
                          const float uv[4], uint32_t tint, uint32_t xform, uint32_t clip)
{
    if (img == nullptr || !(dest[2] > 0) || !(dest[3] > 0)) {
        return;
    }
    float uw = uv[2] - uv[0], uh = uv[3] - uv[1];
    if (!(uw > 0) || !(uh > 0)) {
        return;
    }
    /* With a viewBox the content is laid out in the drawn viewport (dest) by the root's viewBox and preserveAspectRatio, so a
     * square viewBox drawn into a 2:1 box is letterboxed rather than stretched. Without one the natural-size drawing is
     * scaled to the drawn size. */
    float vw = img->w, vh = img->h, sx = dest[2] / (uw * img->w), sy = dest[3] / (uh * img->h);
    if (img->has_vb) {
        vw = dest[2] / uw;
        vh = dest[3] / uh;
        sx = sy = 1.0f;
    }
    embi_svg_env e;
    memset(&e, 0, sizeof e);
    if (env != nullptr) {
        e.dev_scale = env->dev_scale;
        e.vw = env->vw;
        e.vh = env->vh;
        e.text = env->text;
    }
    data_fwd fwd = {env};
    e.se = img->se;
    e.user = &fwd;
    e.image_size = fwd_image_size;
    e.svg_image = fwd_svg_image;
    svg_ctx c;
    svg_ctx_init(&c, &e, SM_PAINT);
    c.dl = dl;
    c.doc = img->doc;
    float t[6] = {sx, 0, 0, sy, dest[0] - uv[0] * vw * sx, dest[1] - uv[1] * vh * sy};
    if (xform > 0 && xform < dl->xforms.len) {
        m_mul(dl->xforms.data[xform].m, t, c.base);
    } else {
        memcpy(c.base, t, sizeof t);
    }
    c.dev *= (sx + sy) * 0.5f;
    c.root_clip = true;
    /* the drawn rect clips the image */
    embi_dclip k;
    memset(&k, 0, sizeof k);
    memcpy(k.rect, dest, sizeof k.rect);
    k.xform = xform;
    k.parent = clip;
    embi_vec_push(&dl->clips, k);
    c.clip0 = dl->clips.len - 1;
    c.view_root = img->root;
    c.view = img->view;
    uint32_t g = 0;
    bool grouped = (tint >> 24) != 0xFFu;
    if (grouped) {
        embi_ditem it;
        memset(&it, 0, sizeof it);
        it.kind = EMBI_DI_LAYER_BEGIN;
        it.f[0] = (float)(tint >> 24) / 255.0f;
        it.clip = c.clip0;
        embi_vec_push(&dl->items, it);
        g = dl->items.len - 1;
    }
    svg_walk_root(&c, img->root, vw, vh);
    if (grouped) {
        embi_ditem end;
        memset(&end, 0, sizeof end);
        end.kind = EMBI_DI_LAYER_END;
        end.aux = g;
        embi_vec_push(&dl->items, end);
        dl->items.data[g].aux = dl->items.len - 1;
        if (dl->items.len - g - 2 == 0) {
            dl->items.len = g;
        }
    }
    svg_ctx_free(&c);
}
