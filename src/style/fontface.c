/*
 * The font registry side of @font-face (W2d): each source of a face is fetched through the
 * embedder's loader and registered with the font registry (src/text) with its descriptors. This TU
 * includes the text headers only; the CSS side (fontparse.c) fills the descriptor.
 *
 * Not applied yet: font-display, size-adjust, ascent-override, descent-override and line-gap-override.
 * The registry (embi_face_desc) has no field for them, so they are parsed and ignored.
 */
#include "style/fontreg.h"

#include "text/text.h"

#include <string.h>

uint32_t embi_style_font_register(embi_fontdb *fonts,
                                  bool (*load)(void *, embi_str, embi_str, uint8_t **, size_t *),
                                  void *user, embi_str base, const embi_style_font_desc *d)
{
    if (fonts == nullptr || load == nullptr) {
        return 0;
    }
    embi_unicode_range ranges[256];
    if (d->nranges > 256) {
        return 0;
    }
    for (uint32_t i = 0; i < d->nranges; i++) {
        ranges[i].lo = d->ranges[i].lo;
        ranges[i].hi = d->ranges[i].hi;
    }
    embi_face_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.family = d->family;
    desc.weight_min = d->weight_min;
    desc.weight_max = d->weight_max;
    desc.has_style = d->has_style;
    desc.style = d->style == 1 ? EMBI_STYLE_ITALIC
                 : d->style == 2 ? EMBI_STYLE_OBLIQUE
                                 : EMBI_STYLE_NORMAL;
    desc.stretch_min = d->stretch_min;
    desc.stretch_max = d->stretch_max;
    desc.ranges = ranges;
    desc.range_count = d->nranges;
    uint32_t added = 0;
    for (uint32_t i = 0; i < d->nurls && added == 0; i++) {
        uint8_t *data = nullptr;
        size_t len = 0;
        if (!load(user, base, d->urls[i], &data, &len) || data == nullptr) {
            continue;
        }
        /* The registry takes the bytes whether or not the face is added. */
        added = embi_fontdb_add(fonts, data, len, true, &desc);
    }
    return added;
}
