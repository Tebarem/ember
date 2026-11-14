/*
 * HTMLElement and the specific element interfaces that need code (the reflected attributes are
 * generated). The form controls are placeholders until W5e.
 */
#include "bindings/impl.h"
#include "bindings/impl_input.h"
#include "bindings/gen/html.h"
#include "dom/event.h"
#include "layout/layout.h"
#include "style/style.h"

static embi_element *el_of(embi_node *n)
{
    return (embi_element *)n;
}

/* ------------------------------------------------------------------ tabIndex, focus, click */

static bool default_focusable(const embi_node *n)
{
    if (n->extra != EMBI_NS_HTML) {
        return false;
    }
    switch (n->name) {
    case EMBI_ATOM_input: {
        /* type=hidden is never focusable. */
        const embi_attr *t = embi_el_get_attribute((embi_element *)(uintptr_t)n, EMBI_STR("type"));
        if (t != nullptr && t->value.len == 6) {
            embi_sb sb = {0};
            embi_dstr_append_utf8(&sb, t->value);
            bool hidden = SDL_strncasecmp(sb.data, "hidden", 6) == 0;
            embi_sb_free(&sb);
            if (hidden) {
                return false;
            }
        }
        return true;
    }
    case EMBI_ATOM_a:
    case EMBI_ATOM_area:
        return embi_el_has_attribute((embi_element *)n, EMBI_STR("href"));
    case EMBI_ATOM_button:
    case EMBI_ATOM_select:
    case EMBI_ATOM_textarea:
    case EMBI_ATOM_iframe:
    case EMBI_ATOM_summary:
        return true;
    default:
        return false;
    }
}

/* ASCII whitespace of the HTML integer parse. */
static bool embi_html_space(uint16_t u)
{
    return u == ' ' || u == '\t' || u == '\n' || u == '\f' || u == '\r';
}

/* The tabindex attribute parsed as the HTML integer rules (leading ASCII whitespace skipped, sign, digits; the rest
 * ignored). False when the attribute is absent or holds no digits: an invalid value counts as absent. */
static bool parse_tabindex(const embi_node *n, int32_t *out)
{
    const embi_attr *a = embi_el_get_attribute(el_of((embi_node *)(uintptr_t)n), EMBI_STR("tabindex"));
    if (a == nullptr) {
        return false;
    }
    uint32_t i = 0;
    while (i < a->value.len && embi_html_space(embi_dstr_unit(a->value, i))) {
        i++;
    }
    bool neg = false;
    if (i < a->value.len && (embi_dstr_unit(a->value, i) == '-' || embi_dstr_unit(a->value, i) == '+')) {
        neg = embi_dstr_unit(a->value, i) == '-';
        i++;
    }
    int64_t v = 0;
    bool any = false;
    for (; i < a->value.len; i++) {
        uint16_t u = embi_dstr_unit(a->value, i);
        if (u < '0' || u > '9') {
            break;
        }
        if (v < 2147483647) {
            v = v * 10 + (u - '0');
        }
        any = true;
    }
    if (!any) {
        return false;
    }
    if (v > 2147483647) {
        v = 2147483647;
    }
    *out = (int32_t)(neg ? -v : v);
    return true;
}

int32_t embi_impl_HTMLElement_get_tabIndex(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    int32_t v = 0;
    if (parse_tabindex(self, &v)) {
        return v;
    }
    return default_focusable(self) ? 0 : -1;
}

void embi_impl_HTMLElement_set_tabIndex(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    char buf[16];
    int n = SDL_snprintf(buf, sizeof buf, "%d", (int)a0);
    (void)embi_el_set_attribute(el_of(self), EMBI_STR("tabindex"), embi_bind_tmp_utf8(c, (embi_str){buf, (uint32_t)n}));
}

void embi_impl_HTMLElement_focus(embi_bind_ctx *c, embi_node *self)
{
    /* The focus change algorithm fires blur, focusout, focus and focusin (impl_input.c). The focusable-area rules are the
     * platform layer's (disabled controls and fieldsets, hidden inputs, a valid tabindex), registered by the input layer. */
    if ((self->flags & EMBI_NF_CONNECTED) && embi_is_element(self) && embi_impl_input_can_focus((const embi_element *)self)) {
        embi_impl_input_focus(c, self);
    }
}

void embi_impl_HTMLElement_blur(embi_bind_ctx *c, embi_node *self)
{
    if (c->active == self) {
        embi_impl_input_focus(c, nullptr);
    }
}

void embi_impl_HTMLElement_click(embi_bind_ctx *c, embi_node *self)
{
    /* click(): a synthetic click that bubbles and is cancelable, not trusted (dom/event.h). The
     * activation behavior of the element (registered with embi_bind_register_activation, see
     * bindings/activation.h) runs from the dispatch. */
    embi_bind_obj *rec = embi_impl_node_rec(c, self);
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_MouseEvent, "click",
                                            EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE | EMBI_EVF_COMPOSED,
                                            EMBI_JS_UNDEFINED);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return;
    }
    embi_bind_pin(c, ev);
    (void)embi_impl_dispatch_value(c, rec, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

/* ------------------------------------------------------------------ geometry */

static bool offset_of(embi_bind_ctx *c, embi_node *self, const embi_element **parent, float *x, float *y,
                      float *w, float *h)
{
    embi_layout_engine *e = embi_bind_layout(c);
    if (!e || !embi_layout_document(e, c->doc)) {
        return false;
    }
    return embi_layout_offset(e, el_of(self), parent, x, y, w, h);
}

embi_node *embi_impl_HTMLElement_get_offsetParent(embi_bind_ctx *c, embi_node *self)
{
    const embi_element *p = nullptr;
    float x, y, w, h;
    if (offset_of(c, self, &p, &x, &y, &w, &h) && p) {
        return (embi_node *)&p->base;
    }
    return nullptr;
}

#define OFFSET_GET(NAME, VAR)                                                                          \
    int32_t embi_impl_HTMLElement_get_##NAME(embi_bind_ctx *c, embi_node *self)                        \
    {                                                                                                  \
        const embi_element *p = nullptr;                                                               \
        float x = 0, y = 0, w = 0, h = 0;                                                              \
        if (offset_of(c, self, &p, &x, &y, &w, &h)) {                                                  \
            return (int32_t)((VAR) + ((VAR) < 0 ? -0.5f : 0.5f));                                      \
        }                                                                                              \
        return 0;                                                                                      \
    }

OFFSET_GET(offsetTop, y)
OFFSET_GET(offsetLeft, x)
OFFSET_GET(offsetWidth, w)
OFFSET_GET(offsetHeight, h)

/* ------------------------------------------------------------------ dataset and style */

embi_js_value embi_impl_HTMLElement_get_dataset(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_dataset_new(c, embi_impl_node_rec(c, self));
}

embi_js_value embi_impl_HTMLElement_get_style(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_style_inline(c, embi_impl_node_rec(c, self));
}

/* ------------------------------------------------------------------ innerText */

static bool block_tag(const embi_node *n)
{
    switch (n->name) {
    case EMBI_ATOM_address:
    case EMBI_ATOM_article:
    case EMBI_ATOM_aside:
    case EMBI_ATOM_blockquote:
    case EMBI_ATOM_body:
    case EMBI_ATOM_dd:
    case EMBI_ATOM_details:
    case EMBI_ATOM_div:
    case EMBI_ATOM_dl:
    case EMBI_ATOM_dt:
    case EMBI_ATOM_fieldset:
    case EMBI_ATOM_figcaption:
    case EMBI_ATOM_figure:
    case EMBI_ATOM_footer:
    case EMBI_ATOM_form:
    case EMBI_ATOM_h1:
    case EMBI_ATOM_h2:
    case EMBI_ATOM_h3:
    case EMBI_ATOM_h4:
    case EMBI_ATOM_h5:
    case EMBI_ATOM_h6:
    case EMBI_ATOM_header:
    case EMBI_ATOM_hr:
    case EMBI_ATOM_li:
    case EMBI_ATOM_main:
    case EMBI_ATOM_nav:
    case EMBI_ATOM_ol:
    case EMBI_ATOM_p:
    case EMBI_ATOM_pre:
    case EMBI_ATOM_section:
    case EMBI_ATOM_table:
    case EMBI_ATOM_tr:
    case EMBI_ATOM_ul:
        return true;
    default:
        return false;
    }
}

/* The display keyword of an element from the style engine, or nullptr when there is no style. */
static bool computed_display(embi_bind_ctx *c, embi_element *el, char *out, size_t cap)
{
    embi_style_engine *se = embi_bind_style(c);
    if (!se) {
        return false;
    }
    embi_style_resolve_document(se, el->base.doc);
    const embi_style *s = embi_style_get(se, el, EMBI_PE_NONE);
    if (!s) {
        return false;
    }
    embi_sb sb = {0};
    bool ok = embi_style_value_text(s, EMBI_PROP_display, &sb);
    if (ok) {
        size_t n = sb.len < cap - 1 ? sb.len : cap - 1;
        if (n) {
            SDL_memcpy(out, sb.data, n);
        }
        out[n] = '\0';
    }
    embi_sb_free(&sb);
    return ok;
}

static void it_newline(embi_sb *sb)
{
    if (sb->len && sb->data[sb->len - 1] != '\n') {
        embi_sb_append_char(sb, '\n');
    }
}

static void it_walk(embi_bind_ctx *c, embi_node *n, embi_sb *sb)
{
    for (embi_node *k = n->first_child; k; k = k->next) {
        if (k->type == EMBI_NODE_TEXT) {
            embi_dstr d = ((embi_chardata *)k)->data;
            embi_sb tmp = {0};
            embi_dstr_append_utf8(&tmp, d);
            bool space = sb->len && (sb->data[sb->len - 1] == ' ' || sb->data[sb->len - 1] == '\n');
            for (uint32_t i = 0; i < tmp.len; i++) {
                char ch = tmp.data[i];
                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
                    if (!space) {
                        embi_sb_append_char(sb, ' ');
                        space = true;
                    }
                } else {
                    embi_sb_append_char(sb, ch);
                    space = false;
                }
            }
            embi_sb_free(&tmp);
        } else if (k->type == EMBI_NODE_ELEMENT) {
            char disp[32] = "";
            bool have = computed_display(c, (embi_element *)k, disp, sizeof disp);
            if (have && SDL_strcmp(disp, "none") == 0) {
                continue;
            }
            if (k->extra == EMBI_NS_HTML && (k->name == EMBI_ATOM_script || k->name == EMBI_ATOM_style)) {
                continue;
            }
            if (k->extra == EMBI_NS_HTML && k->name == EMBI_ATOM_br) {
                embi_sb_append_char(sb, '\n');
                continue;
            }
            bool block = have ? (SDL_strncmp(disp, "inline", 6) != 0 && SDL_strcmp(disp, "contents") != 0 &&
                                 SDL_strncmp(disp, "table-cell", 10) != 0)
                              : (k->extra == EMBI_NS_HTML && block_tag(k));
            if (block) {
                it_newline(sb);
            }
            it_walk(c, k, sb);
            if (block) {
                it_newline(sb);
            }
        }
    }
}

embi_dom_err embi_impl_HTMLElement_get_innerText(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    embi_sb sb = {0};
    it_walk(c, self, &sb);
    while (sb.len && (sb.data[sb.len - 1] == '\n' || sb.data[sb.len - 1] == ' ')) {
        sb.len--;
    }
    uint32_t start = 0;
    while (start < sb.len && (sb.data[start] == '\n' || sb.data[start] == ' ')) {
        start++;
    }
    *out = embi_bind_tmp_utf8(c, (embi_str){sb.data ? sb.data + start : "", sb.len - start});
    embi_sb_free(&sb);
    return EMBI_DOM_OK;
}

/* Replaces the children of target with the text, "\n" becoming <br>. */
static embi_dom_err set_text_with_breaks(embi_bind_ctx *c, embi_node *target, embi_dstr text)
{
    embi_node *frag = embi_new_fragment(target->doc);
    embi_str s = embi_bind_utf8(c, text);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s.len; i++) {
        bool brk = i == s.len || s.ptr[i] == '\n' || s.ptr[i] == '\r';
        if (!brk) {
            continue;
        }
        if (i > start) {
            embi_chardata *t = embi_new_text_utf8(target->doc, (embi_str){s.ptr + start, i - start});
            (void)embi_append_child(frag, &t->base);
        }
        if (i < s.len) {
            embi_element *br = nullptr;
            if (embi_dom_create_element(target->doc, EMBI_STR("br"), &br) == EMBI_DOM_OK) {
                (void)embi_append_child(frag, &br->base);
            }
            if (s.ptr[i] == '\r' && i + 1 < s.len && s.ptr[i + 1] == '\n') {
                i++;
            }
        }
        start = i + 1;
    }
    embi_dom_err e = embi_replace_all(target, frag->first_child ? frag : nullptr);
    embi_destroy_detached(frag);
    return e;
}

embi_dom_err embi_impl_HTMLElement_set_innerText(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    return set_text_with_breaks(c, self, a0);
}

embi_dom_err embi_impl_HTMLElement_get_outerText(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    return embi_impl_HTMLElement_get_innerText(c, self, out);
}

embi_dom_err embi_impl_HTMLElement_set_outerText(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    embi_node *parent = self->parent;
    if (!parent) {
        return embi_impl_throw(c, "NoModificationAllowedError", "The element has no parent.");
    }
    embi_chardata *t = embi_new_text(self->doc, a0);
    embi_dom_err e = embi_replace_child(parent, &t->base, self);
    if (e != EMBI_DOM_OK) {
        embi_destroy_detached(&t->base);
    }
    return e;
}

/* ------------------------------------------------------------------ specific interfaces */

static embi_dom_err text_get(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    /* the concatenation of the child Text nodes */
    embi_dbuf b;
    embi_dbuf_init(&b, self->doc->slab);
    for (embi_node *k = self->first_child; k; k = k->next) {
        if (k->type == EMBI_NODE_TEXT) {
            embi_dbuf_append(&b, ((embi_chardata *)k)->data);
        }
    }
    embi_dstr d = embi_dbuf_finish(&b);
    *out = embi_bind_tmp_copy(c, d);
    embi_dstr_free(d);
    embi_dbuf_free(&b);
    return EMBI_DOM_OK;
}

static embi_dom_err text_set(embi_node *self, embi_dstr v)
{
    return embi_text_content_set(self, v);
}

embi_dom_err embi_impl_HTMLAnchorElement_get_text(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    return text_get(c, self, out);
}

embi_dom_err embi_impl_HTMLAnchorElement_set_text(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    return text_set(self, a0);
}

embi_dom_err embi_impl_HTMLScriptElement_get_text(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    return text_get(c, self, out);
}

embi_dom_err embi_impl_HTMLScriptElement_set_text(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    return text_set(self, a0);
}

embi_dom_err embi_impl_HTMLTitleElement_get_text(embi_bind_ctx *c, embi_node *self, embi_dstr *out)
{
    return text_get(c, self, out);
}

embi_dom_err embi_impl_HTMLTitleElement_set_text(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    return text_set(self, a0);
}

embi_js_value embi_impl_HTMLAnchorElement_get_relList(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_tokenlist_new(c, embi_impl_node_rec(c, self), EMBI_STR("rel"));
}

embi_js_value embi_impl_HTMLLinkElement_get_relList(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_tokenlist_new(c, embi_impl_node_rec(c, self), EMBI_STR("rel"));
}

embi_node *embi_impl_HTMLTemplateElement_get_content(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return embi_template_contents(el_of(self));
}

/* The Image factory: new Image(width, height) is an <img> with those attributes (the legacy factory
 * function of HTMLImageElement, HTML Standard 4.8.3). */
static embi_js_value image_factory(embi_js_realm *r, embi_js_value this_v, int argc, const embi_js_value *argv,
                                   int magic, embi_js_value new_target)
{
    (void)this_v;
    (void)magic;
    embi_js_runtime *rt = r->rt;
    if (embi_js_is_undefined(new_target)) {
        return embi_js_throw_type_error(rt, "Failed to construct 'Image': Please use the 'new' operator, this DOM object constructor cannot be called as a function.");
    }
    embi_bind_ctx *c = embi_bind_ctx_of(r);
    if (!c) {
        return embi_js_throw_type_error(rt, "Illegal invocation");
    }
    embi_bind_scope sc = embi_bind_enter(c);
    int64_t dim[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (i < argc && !embi_js_is_undefined(argv[i]) &&
            !embi_bind_conv_int(c, argv[i], 6, "Failed to construct 'Image'", &dim[i])) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
    }
    embi_element *img = nullptr;
    if (embi_dom_create_element(c->doc, EMBI_STR("img"), &img) != EMBI_DOM_OK) {
        return embi_bind_leave(sc, embi_js_throw_type_error(rt, "Failed to construct 'Image'"));
    }
    for (int i = 0; i < 2; i++) {
        if (i < argc && !embi_js_is_undefined(argv[i])) {
            char buf[24];
            int n = SDL_snprintf(buf, sizeof buf, "%u", (unsigned)dim[i]);
            (void)embi_el_set_attribute(img, i == 0 ? EMBI_STR("width") : EMBI_STR("height"),
                                        embi_bind_tmp_utf8(c, (embi_str){buf, (uint32_t)n}));
        }
    }
    embi_js_value v = embi_bind_node_value(c, &img->base);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

void embi_impl_html_install_image(embi_bind_ctx *c)
{
    embi_js_realm *r = c->realm;
    embi_js_runtime *rt = c->rt;
    embi_js_object *ctor = embi_js_native_constructor_new(r, "Image", 0, image_factory, 0);
    embi_js_define_raw(rt, ctor, embi_js_key_c(rt, "prototype"), c->protos[EMBI_IF_HTMLImageElement], 0);
    embi_js_define_raw(rt, r->global, embi_js_key_c(rt, "Image"), embi_js_from_object(ctor),
                       EMBI_JS_ATTR_WRITABLE | EMBI_JS_ATTR_CONFIGURABLE);
}

/* The natural size of the image of an img element through the host hook. False without a hook, a src, or a loaded image. */
static bool img_natural(embi_bind_ctx *c, embi_node *self, float *w, float *h)
{
    if (c->host.image_size == nullptr) {
        return false;
    }
    const embi_attr *a = embi_el_get_attribute(el_of(self), EMBI_STR("src"));
    if (a == nullptr || a->value.len == 0) {
        return false;
    }
    embi_sb sb = {0};
    embi_dstr_append_utf8(&sb, a->value);
    bool ok = c->host.image_size(c->host.user, (embi_str){sb.data, sb.len}, w, h);
    embi_sb_free(&sb);
    return ok;
}

uint32_t embi_impl_HTMLImageElement_get_naturalWidth(embi_bind_ctx *c, embi_node *self)
{
    float w = 0, h = 0;
    return img_natural(c, self, &w, &h) && w > 0 ? (uint32_t)(w + 0.5f) : 0;
}

uint32_t embi_impl_HTMLImageElement_get_naturalHeight(embi_bind_ctx *c, embi_node *self)
{
    float w = 0, h = 0;
    return img_natural(c, self, &w, &h) && h > 0 ? (uint32_t)(h + 0.5f) : 0;
}

bool embi_impl_HTMLImageElement_get_complete(embi_bind_ctx *c, embi_node *self)
{
    const embi_attr *a = embi_el_get_attribute(el_of(self), EMBI_STR("src"));
    /* loads are synchronous: with a size hook, an image with a src has finished loading (it failed when it has no size) */
    if (a == nullptr || a->value.len == 0) {
        return true;
    }
    if (c->host.image_pending != nullptr) {
        embi_sb sb = {0};
        embi_dstr_append_utf8(&sb, a->value);
        bool pending = c->host.image_pending(c->host.user, (embi_str){sb.data, sb.len});
        embi_sb_free(&sb);
        if (pending) {
            return false; /* a network image that has not arrived */
        }
    }
    return c->host.image_size != nullptr;
}

embi_dstr embi_impl_HTMLImageElement_get_currentSrc(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    const embi_attr *a = embi_el_get_attribute(el_of(self), EMBI_STR("src"));
    return a ? a->value : embi_dstr_empty();
}
