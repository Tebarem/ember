/*
 * The members of SVGElement that do not need the SVG feature (svgelement.webidl): dataset, style, tabIndex, focus and blur. Split from
 * impl_svg.c by SZ3b so that they exist in a build with EMBER_SVG=OFF too.
 */
#include "bindings/impl.h"
#include "bindings/gen/dom.h"
#include "bindings/gen/html.h"
#include "bindings/gen/svgelement.h"

#include <stdint.h>

static bool svg_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

embi_js_value embi_impl_SVGElement_get_dataset(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_dataset_new(c, embi_impl_node_rec(c, self));
}

embi_js_value embi_impl_SVGElement_get_style(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_style_inline(c, embi_impl_node_rec(c, self));
}

/* tabIndex: the tabindex attribute as an integer; -1 when it is absent or invalid (SVG elements are not focusable by
 * default). */
int32_t embi_impl_SVGElement_get_tabIndex(embi_bind_ctx *c, embi_node *self)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)self, EMBI_STR("tabindex"));
    if (a == nullptr) {
        return -1;
    }
    embi_str v = embi_bind_utf8(c, a->value);
    uint32_t i = 0;
    bool neg = false;
    while (i < v.len && svg_ws(v.ptr[i])) {
        i++;
    }
    if (i < v.len && (v.ptr[i] == '-' || v.ptr[i] == '+')) {
        neg = v.ptr[i] == '-';
        i++;
    }
    if (i >= v.len || v.ptr[i] < '0' || v.ptr[i] > '9') {
        return -1;
    }
    int64_t n = 0;
    while (i < v.len && v.ptr[i] >= '0' && v.ptr[i] <= '9') {
        if (n < 100000000) {
            n = n * 10 + (v.ptr[i] - '0');
        }
        i++;
    }
    n = neg ? -n : n;
    if (n > INT32_MAX) {
        n = INT32_MAX;
    }
    if (n < INT32_MIN) {
        n = INT32_MIN;
    }
    return (int32_t)n;
}

void embi_impl_SVGElement_set_tabIndex(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    char buf[16];
    int n = SDL_snprintf(buf, sizeof buf, "%d", (int)a0);
    (void)embi_el_set_attribute((embi_element *)self, EMBI_STR("tabindex"),
                                embi_bind_tmp_utf8(c, (embi_str){buf, (uint32_t)n}));
}

void embi_impl_SVGElement_focus(embi_bind_ctx *c, embi_node *self)
{
    embi_impl_HTMLElement_focus(c, self);
}

void embi_impl_SVGElement_blur(embi_bind_ctx *c, embi_node *self)
{
    embi_impl_HTMLElement_blur(c, self);
}
