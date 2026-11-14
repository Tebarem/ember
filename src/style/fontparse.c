/*
 * @font-face parsing (W2d, CSS side): the descriptors of a rule (font-family, src, font-weight, font-style,
 * font-stretch, unicode-range) are read from the rule's raw declarations, each source is fetched
 * through the embedder's font loader (with the sheet's base URL), and the bytes are registered with
 * the font registry (src/text) with the descriptors.
 *
 * An invalid descriptor is ignored, as the CSS Fonts rules say: a unicode-range that is not a list
 * of ranges (or has more than FACE_MAX_RANGES of them) leaves the face covering every code point.
 *
 * Not applied yet (W6/R2c follow-up): font-display, size-adjust, ascent-override, descent-override and
 * line-gap-override: embi_face_desc has no fields for them yet, so they are ignored here.
 */
#include "style_int.h"
#include "style/fontreg.h"

#include <string.h>

#define FACE_MAX_SRC 16u
#define FACE_MAX_RANGES 256u

typedef struct face_src {
    embi_str url;
} face_src;

static const embi_css_decl *find_desc(const embi_css_rule *r, const char *name)
{
    embi_str want = {name, (uint32_t)strlen(name)};
    const embi_css_decl *found = nullptr;
    for (uint32_t i = 0; i < r->ndecls; i++) {
        if (embi_str_ieq(embi_atom_str(r->decls[i].name), want)) {
            found = &r->decls[i];
        }
    }
    return found;
}

static uint32_t next_nonws(embi_css_slice s, uint32_t i)
{
    while (i < s.n && s.p[i].tok.type == EMBI_CSS_WS) {
        i++;
    }
    return i;
}

static bool format_supported(embi_str f)
{
    static const char *const ok[] = {"truetype", "opentype", "woff", "woff2", "collection"};
    for (uint32_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        if (embi_str_ieq(f, (embi_str){ok[i], (uint32_t)strlen(ok[i])})) {
            return true;
        }
    }
    return false;
}

/* The family name of a font-family descriptor: a single string, or one or more identifiers joined
 * with single spaces (CSS Fonts 4, <family-name>). The result lives in the arena. */
static bool family_name(embi_arena *arena, embi_css_slice s, embi_str *out)
{
    uint32_t i = next_nonws(s, 0);
    if (i >= s.n) {
        return false;
    }
    if (s.p[i].tok.type == EMBI_CSS_STRING) {
        *out = embi_style_copy_str(arena, s.p[i].tok.value);
        return out->len > 0;
    }
    uint32_t total = 0;
    uint32_t words = 0;
    uint32_t k = i;
    while (k < s.n && s.p[k].tok.type != EMBI_CSS_COMMA) {
        if (s.p[k].tok.type == EMBI_CSS_IDENT) {
            total += s.p[k].tok.value.len + (words > 0 ? 1u : 0u);
            words++;
        } else if (s.p[k].tok.type != EMBI_CSS_WS) {
            return false;
        }
        k++;
    }
    if (words == 0) {
        return false;
    }
    char *buf = embi_arena_alloc(arena, total + 1, 1);
    uint32_t w = 0;
    bool first = true;
    for (uint32_t j = i; j < k; j++) {
        if (s.p[j].tok.type != EMBI_CSS_IDENT) {
            continue;
        }
        if (!first) {
            buf[w++] = ' ';
        }
        memcpy(buf + w, s.p[j].tok.value.ptr, s.p[j].tok.value.len);
        w += s.p[j].tok.value.len;
        first = false;
    }
    buf[w] = '\0';
    *out = (embi_str){buf, w};
    return true;
}

/* Parses the src descriptor. Each comma-separated source is url() or local(), optionally followed
 * by format() (an unsupported format drops the source). tech() is ignored. */
static uint32_t parse_src(embi_arena *arena, embi_css_slice s, face_src *out, uint32_t cap)
{
    uint32_t n = 0;
    uint32_t i = 0;
    while (i < s.n && n < cap) {
        /* One source: up to the next top-level comma. */
        uint32_t end = i;
        while (end < s.n && s.p[end].tok.type != EMBI_CSS_COMMA) {
            end++;
        }
        embi_css_slice part = embi_css_slice_make(s.p + i, end - i);
        embi_str url = {nullptr, 0};
        bool fmt_ok = true;
        for (uint32_t k = next_nonws(part, 0); k < part.n; k = next_nonws(part, k + 1)) {
            const embi_css_cv *cv = &part.p[k];
            if (cv->tok.type == EMBI_CSS_URL) {
                url = cv->tok.value;
            } else if (cv->tok.type == EMBI_CSS_FUNCTION) {
                embi_css_slice kids = embi_css_slice_make(cv->kids, cv->nkids);
                uint32_t j = next_nonws(kids, 0);
                embi_str arg = {nullptr, 0};
                if (j < kids.n && (kids.p[j].tok.type == EMBI_CSS_STRING ||
                                   kids.p[j].tok.type == EMBI_CSS_IDENT ||
                                   kids.p[j].tok.type == EMBI_CSS_URL)) {
                    arg = kids.p[j].tok.value;
                }
                if (embi_str_ieq(cv->tok.value, EMBI_STR("url"))) {
                    url = arg;
                } else if (embi_str_ieq(cv->tok.value, EMBI_STR("local")) && arg.len > 0) {
                    /* local(name): the loader gets the name with a "local:" scheme. */
                    char *buf = embi_arena_alloc(arena, arg.len + 7, 1);
                    memcpy(buf, "local:", 6);
                    memcpy(buf + 6, arg.ptr, arg.len);
                    buf[arg.len + 6] = '\0';
                    url = (embi_str){buf, arg.len + 6};
                } else if (embi_str_ieq(cv->tok.value, EMBI_STR("format"))) {
                    fmt_ok = arg.len > 0 && format_supported(arg);
                }
            }
        }
        if (url.len > 0 && fmt_ok) {
            out[n].url = embi_style_copy_str(arena, url);
            n++;
        }
        i = end + 1;
    }
    return n;
}

/* Parses a unicode-range list into out (at most FACE_MAX_RANGES ranges). Returns false, and sets
 * nothing, when the descriptor is not a valid list of ranges. */
static bool parse_unicode_range(embi_css_slice s, embi_style_range *out, uint32_t *count)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s.n; i++) {
        const embi_css_cv *cv = &s.p[i];
        if (cv->tok.type == EMBI_CSS_UNICODE_RANGE) {
            if (n == FACE_MAX_RANGES) {
                return false;
            }
            out[n].lo = cv->tok.cp;
            out[n].hi = cv->tok.cp_end;
            n++;
        } else if (cv->tok.type != EMBI_CSS_WS && cv->tok.type != EMBI_CSS_COMMA) {
            return false;
        }
    }
    if (n == 0) {
        return false;
    }
    *count = n;
    return true;
}

static bool parse_weight(embi_css_slice v, uint16_t *lo, uint16_t *hi)
{
    uint32_t i = next_nonws(v, 0);
    if (i >= v.n) {
        return false;
    }
    const embi_css_token *t = &v.p[i].tok;
    if (t->type == EMBI_CSS_IDENT && embi_str_ieq(t->value, EMBI_STR("normal"))) {
        *lo = *hi = 400;
        return true;
    }
    if (t->type == EMBI_CSS_IDENT && embi_str_ieq(t->value, EMBI_STR("bold"))) {
        *lo = *hi = 700;
        return true;
    }
    if (t->type != EMBI_CSS_NUMBER) {
        return false;
    }
    double a = t->num;
    double b = a;
    uint32_t j = next_nonws(v, i + 1);
    if (j < v.n && v.p[j].tok.type == EMBI_CSS_NUMBER) {
        b = v.p[j].tok.num;
    }
    *lo = (uint16_t)(a < 1 ? 1 : a > 1000 ? 1000 : a);
    *hi = (uint16_t)(b < 1 ? 1 : b > 1000 ? 1000 : b);
    return true;
}

void embi_style_font_face_rule(embi_style_engine *e, embi_str base, const embi_css_rule *r)
{
    embi_arena *arena = &e->arena;
    embi_fontdb *fonts = e->fonts;
    if (fonts == nullptr) {
        return;
    }
    /* A sheet registers again when a source of one of its faces was still on its way: the faces that are done stay. */
    uint64_t key = (uint64_t)(uintptr_t)r;
    uint64_t seen = 0;
    if (embi_hashmap_get(&e->face_done, key, &seen)) {
        return;
    }
    const embi_css_decl *fam = find_desc(r, "font-family");
    embi_str family = {nullptr, 0};
    if (fam == nullptr || !family_name(arena, fam->value, &family)) {
        return; /* a face needs a family name */
    }

    const embi_css_decl *srcd = find_desc(r, "src");
    if (srcd == nullptr) {
        return;
    }
    face_src srcs[FACE_MAX_SRC];
    uint32_t nsrc = parse_src(arena, srcd->value, srcs, FACE_MAX_SRC);

    embi_style_font_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.family = family;

    const embi_css_decl *wd = find_desc(r, "font-weight");
    if (wd != nullptr) {
        uint16_t lo = 0;
        uint16_t hi = 0;
        if (parse_weight(wd->value, &lo, &hi)) {
            desc.weight_min = lo;
            desc.weight_max = hi;
        }
    }

    const embi_css_decl *sd = find_desc(r, "font-style");
    if (sd != nullptr) {
        uint32_t i = next_nonws(sd->value, 0);
        if (i < sd->value.n && sd->value.p[i].tok.type == EMBI_CSS_IDENT) {
            embi_str kw = sd->value.p[i].tok.value;
            desc.has_style = true;
            if (embi_str_ieq(kw, EMBI_STR("normal"))) {
                desc.style = 0;
            } else if (embi_str_ieq(kw, EMBI_STR("italic"))) {
                desc.style = 1;
            } else if (embi_str_ieq(kw, EMBI_STR("oblique"))) {
                desc.style = 2;
            } else {
                desc.has_style = false;
            }
        }
    }

    const embi_css_decl *std = find_desc(r, "font-stretch");
    if (std != nullptr) {
        uint32_t i = next_nonws(std->value, 0);
        if (i < std->value.n) {
            const embi_css_token *t = &std->value.p[i].tok;
            if (t->type == EMBI_CSS_PERCENTAGE) {
                desc.stretch_min = (float)t->num;
                desc.stretch_max = (float)t->num;
                uint32_t j = next_nonws(std->value, i + 1);
                if (j < std->value.n && std->value.p[j].tok.type == EMBI_CSS_PERCENTAGE) {
                    desc.stretch_max = (float)std->value.p[j].tok.num;
                }
            }
        }
    }

    const embi_css_decl *ur = find_desc(r, "unicode-range");
    if (ur != nullptr) {
        embi_style_range tmp[FACE_MAX_RANGES];
        uint32_t count = 0;
        if (parse_unicode_range(ur->value, tmp, &count)) {
            embi_style_range *kept = embi_arena_alloc(arena, sizeof(embi_style_range) * count, 8);
            memcpy(kept, tmp, sizeof(embi_style_range) * count);
            desc.ranges = kept;
            desc.nranges = count;
        }
    }

    embi_str urls[FACE_MAX_SRC];
    for (uint32_t i = 0; i < nsrc; i++) {
        urls[i] = srcs[i].url;
    }
    desc.urls = urls;
    desc.nurls = nsrc;
    bool was_pending = e->font_pending;
    e->font_pending = false;
    (void)embi_style_font_register(fonts, e->cb.load_font, e->cb.user, base, &desc);
    if (!e->font_pending) {
        embi_hashmap_put(&e->face_done, key, 1);
    }
    e->font_pending = e->font_pending || was_pending;
}
