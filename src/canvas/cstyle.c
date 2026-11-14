/*
 * Canvas style strings (R4b): CSS colors, the font shorthand and lengths. This is the only canvas file that sees the CSS
 * headers.
 */
#include "canvas/canvas_int.h"

#include "css/css.h"
#include "css/props.h"

#include <string.h>

/* ------------------------------------------------------------------ colors */

bool cs_parse_color(embi_canvas *cv, embi_str css, uint8_t rgb[3], double *alpha)
{
    if (css.len == 0) {
        return false;
    }
    embi_arena a;
    embi_arena_init(&a, 2048);
    embi_css_slice sl = embi_css_parse_cvs(&a, css);
    embi_color c;
    bool ok = embi_color_parse(sl, &c);
    if (ok) {
        if (c.space == EMBI_CS_CURRENT) {
            float fpx, root;
            uint32_t col;
            bool rtl;
            cv_element_style(cv, &fpx, &col, &rtl, &root);
            rgb[0] = (uint8_t)(col & 0xFF);
            rgb[1] = (uint8_t)((col >> 8) & 0xFF);
            rgb[2] = (uint8_t)((col >> 16) & 0xFF);
            *alpha = (double)(col >> 24) / 255.0;
        } else if (c.space == EMBI_CS_SYSTEM || c.space == EMBI_CS_DEFERRED) {
            ok = false;
        } else {
            uint8_t px[4];
            ok = embi_color_to_srgb8(&c, px);
            if (ok) {
                rgb[0] = px[0];
                rgb[1] = px[1];
                rgb[2] = px[2];
                double al = c.alpha;
                if (al != al) {
                    al = 0.0;
                }
                *alpha = al < 0.0 ? 0.0 : (al > 1.0 ? 1.0 : al);
            }
        }
    }
    embi_arena_free(&a);
    return ok;
}

void cs_color_string(const uint8_t rgb[3], double alpha, embi_sb *out)
{
    char buf[96];
    if (alpha >= 1.0) {
        SDL_snprintf(buf, sizeof buf, "#%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
    } else {
        /* The alpha with the fewest digits that survives the 8-bit round trip: two decimals, else three. */
        int target = (int)(alpha * 255.0 + 0.5);
        char ab[24];
        double r2 = (double)(int)(alpha * 100.0 + 0.5) / 100.0;
        if ((int)(r2 * 255.0 + 0.5) == target) {
            SDL_snprintf(ab, sizeof ab, "%.2f", r2);
        } else {
            SDL_snprintf(ab, sizeof ab, "%.3f", alpha);
        }
        size_t n = strlen(ab);
        while (n > 1 && ab[n - 1] == '0') {
            ab[--n] = '\0';
        }
        if (n > 0 && ab[n - 1] == '.') {
            ab[--n] = '\0';
        }
        SDL_snprintf(buf, sizeof buf, "rgba(%u, %u, %u, %s)", rgb[0], rgb[1], rgb[2], ab);
    }
    embi_sb_append(out, (embi_str){buf, (uint32_t)strlen(buf)});
}

/* ------------------------------------------------------------------ names */

static const char *const k_stretch[9] = {"ultra-condensed", "extra-condensed", "condensed", "semi-condensed", "normal",
                                         "semi-expanded",   "expanded",        "extra-expanded", "ultra-expanded"};
static const char *const k_caps[7] = {"normal", "small-caps", "all-small-caps", "petite-caps", "all-petite-caps",
                                      "unicase", "titling-caps"};
static const char *const k_render[4] = {"auto", "optimizeSpeed", "optimizeLegibility", "geometricPrecision"};

const char *cs_font_stretch_name(uint8_t i)
{
    return k_stretch[i < 9 ? i : 4];
}

const char *cs_caps_name(uint8_t i)
{
    return k_caps[i < 7 ? i : 0];
}

const char *cs_rendering_name(uint8_t i)
{
    return k_render[i < 4 ? i : 0];
}

/* ------------------------------------------------------------------ lengths and fonts */

static char *dup_n(const char *s, size_t n)
{
    char *d = embi_alloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

void cfont_free(cfont *f)
{
    if (f->css != nullptr) {
        embi_free(f->css, strlen(f->css) + 1);
    }
    if (f->family != nullptr) {
        embi_free(f->family, strlen(f->family) + 1);
    }
    memset(f, 0, sizeof *f);
}

void cfont_copy(cfont *dst, const cfont *src)
{
    *dst = *src;
    dst->css = src->css != nullptr ? dup_n(src->css, strlen(src->css)) : nullptr;
    dst->family = src->family != nullptr ? dup_n(src->family, strlen(src->family)) : nullptr;
}

/* The factor from a CSS length unit to px, with em and rem resolved; false for an unknown unit. */
static bool unit_scale(const char *u, size_t n, double base_px, double root_px, double *k)
{
#define U(name, val)                                                                                \
    if (n == sizeof(name) - 1 && SDL_strncasecmp(u, name, n) == 0) {                                \
        *k = (val);                                                                                 \
        return true;                                                                                \
    }
    U("px", 1.0)
    U("em", base_px)
    U("rem", root_px)
    U("ex", base_px * 0.5)
    U("ch", base_px * 0.5)
    U("pt", 96.0 / 72.0)
    U("pc", 16.0)
    U("in", 96.0)
    U("cm", 96.0 / 2.54)
    U("mm", 96.0 / 25.4)
    U("q", 96.0 / 101.6)
#undef U
    return false;
}

/* A number with an optional unit at s (up to n bytes). Returns the bytes used, 0 on failure. */
static size_t scan_number(const char *s, size_t n, double *v, char unit[16], size_t *ulen)
{
    size_t i = 0;
    char buf[64];
    size_t b = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        buf[b++] = s[i++];
    }
    bool digits = false;
    while (i < n && s[i] >= '0' && s[i] <= '9' && b < 60) {
        buf[b++] = s[i++];
        digits = true;
    }
    if (i < n && s[i] == '.') {
        buf[b++] = s[i++];
        while (i < n && s[i] >= '0' && s[i] <= '9' && b < 60) {
            buf[b++] = s[i++];
            digits = true;
        }
    }
    if (!digits) {
        return 0;
    }
    if (i + 1 < n && (s[i] == 'e' || s[i] == 'E') &&
        ((s[i + 1] >= '0' && s[i + 1] <= '9') || ((s[i + 1] == '+' || s[i + 1] == '-') && i + 2 < n && s[i + 2] >= '0' && s[i + 2] <= '9'))) {
        buf[b++] = s[i++];
        buf[b++] = s[i++];
        while (i < n && s[i] >= '0' && s[i] <= '9' && b < 62) {
            buf[b++] = s[i++];
        }
    }
    buf[b] = '\0';
    *v = SDL_strtod(buf, nullptr);
    size_t u = 0;
    while (i + u < n && (((s[i + u] | 0x20) >= 'a' && (s[i + u] | 0x20) <= 'z') || s[i + u] == '%') && u < 15) {
        unit[u] = s[i + u];
        u++;
    }
    unit[u] = '\0';
    *ulen = u;
    return i + u;
}

bool cs_parse_length(embi_str css, float base_px, float root_px, double *out)
{
    const char *s = css.ptr;
    size_t n = css.len;
    while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\n')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n')) {
        n--;
    }
    double v;
    char unit[16];
    size_t ul;
    size_t used = scan_number(s, n, &v, unit, &ul);
    if (used == 0 || used != n || !isfinite(v)) {
        return false;
    }
    if (ul == 0) {
        if (v != 0.0) {
            return false;
        }
        *out = 0.0;
        return true;
    }
    double k;
    if (!unit_scale(unit, ul, base_px, root_px, &k)) {
        return false;
    }
    *out = v * k;
    return true;
}

typedef struct ftok {
    const char *s;
    size_t n;
    bool quoted;
} ftok;

static bool ieq(const ftok *t, const char *lit)
{
    return t->n == strlen(lit) && !t->quoted && SDL_strncasecmp(t->s, lit, t->n) == 0;
}

static bool is_generic(const char *s, size_t n)
{
    static const char *const g[] = {"serif", "sans-serif", "monospace", "cursive", "fantasy", "system-ui", "math",
                                    "ui-serif", "ui-sans-serif", "ui-monospace", "ui-rounded", "emoji", "fangsong"};
    for (size_t i = 0; i < sizeof g / sizeof *g; i++) {
        if (n == strlen(g[i]) && SDL_strncasecmp(s, g[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* font: [style || variant || weight || stretch]? size[/line-height]? families. */
bool cs_parse_font(embi_str css, float base_px, float root_px, cfont *out)
{
    memset(out, 0, sizeof *out);
    const char *p = css.ptr, *e = css.ptr + css.len;
    ftok toks[40];
    int nt = 0;
    int size_at = -1;
    /* Tokenize up to the size: words separated by spaces. The size token is the first one that starts like a number or is
     * a size keyword, and everything after it is the family list. */
    out->weight = 400;
    out->stretch = 100;
    bool got_style = false, got_var = false, got_weight = false, got_stretch = false;
    int normals = 0;
    while (true) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) {
            p++;
        }
        if (p >= e) {
            break;
        }
        const char *st = p;
        while (p < e && !(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f') && *p != '/') {
            p++;
        }
        if (p == st) {
            break; /* a '/' */
        }
        ftok t = {st, (size_t)(p - st), false};
        bool is_kw = ieq(&t, "normal") || ieq(&t, "italic") || ieq(&t, "oblique") || ieq(&t, "small-caps") ||
                     ieq(&t, "bold") || ieq(&t, "bolder") || ieq(&t, "lighter");
        bool stretch_kw = false;
        for (int i = 0; i < 9; i++) {
            if (i != 4 && ieq(&t, k_stretch[i])) {
                stretch_kw = true;
            }
        }
        double v;
        char unit[16];
        size_t ul;
        bool weight_num = scan_number(t.s, t.n, &v, unit, &ul) == t.n && ul == 0 && v >= 1 && v <= 1000;
        if (is_kw || stretch_kw || weight_num) {
            if (nt >= 8) {
                return false;
            }
            if (ieq(&t, "normal")) {
                normals++;
                if (normals > 3) {
                    return false;
                }
            } else if (ieq(&t, "italic") || ieq(&t, "oblique")) {
                if (got_style) {
                    return false;
                }
                got_style = true;
                out->style = ieq(&t, "italic") ? 1 : 2;
                if (ieq(&t, "oblique")) {
                    /* An optional angle. */
                    const char *q = p;
                    while (q < e && *q == ' ') {
                        q++;
                    }
                    double av;
                    char au[16];
                    size_t aul;
                    const char *qs = q;
                    while (q < e && *q != ' ' && *q != '/') {
                        q++;
                    }
                    if (q > qs && scan_number(qs, (size_t)(q - qs), &av, au, &aul) == (size_t)(q - qs) &&
                        (SDL_strncasecmp(au, "deg", 3) == 0 && aul == 3)) {
                        p = q;
                    }
                }
            } else if (ieq(&t, "small-caps")) {
                if (got_var) {
                    return false;
                }
                got_var = true;
                out->small_caps = true;
            } else if (stretch_kw) {
                if (got_stretch) {
                    return false;
                }
                got_stretch = true;
                static const float pct[9] = {50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200};
                for (int i = 0; i < 9; i++) {
                    if (ieq(&t, k_stretch[i])) {
                        out->stretch = pct[i];
                    }
                }
            } else {
                if (got_weight) {
                    return false;
                }
                got_weight = true;
                if (ieq(&t, "bold")) {
                    out->weight = 700;
                } else if (ieq(&t, "bolder")) {
                    out->weight = 700;
                } else if (ieq(&t, "lighter")) {
                    out->weight = 100;
                } else {
                    out->weight = (float)v;
                }
            }
            nt++;
            continue;
        }
        size_at = nt;
        toks[nt++] = t;
        break;
    }
    if (size_at < 0) {
        return false;
    }
    /* The size. */
    ftok sz = toks[size_at];
    double px = 0.0;
    static const struct {
        const char *n;
        double px;
    } abs_sizes[] = {{"xx-small", 9}, {"x-small", 10}, {"small", 13},  {"medium", 16},
                     {"large", 18},   {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
    bool found = false;
    for (size_t i = 0; i < sizeof abs_sizes / sizeof *abs_sizes; i++) {
        if (ieq(&sz, abs_sizes[i].n)) {
            px = abs_sizes[i].px;
            found = true;
        }
    }
    if (!found && ieq(&sz, "larger")) {
        px = base_px * 1.2;
        found = true;
    } else if (!found && ieq(&sz, "smaller")) {
        px = base_px / 1.2;
        found = true;
    }
    if (!found) {
        double v;
        char unit[16];
        size_t ul;
        if (scan_number(sz.s, sz.n, &v, unit, &ul) != sz.n || ul == 0 || v < 0) {
            return false;
        }
        if (ul == 1 && unit[0] == '%') {
            px = base_px * v / 100.0;
        } else {
            double k;
            if (!unit_scale(unit, ul, base_px, root_px, &k)) {
                return false;
            }
            px = v * k;
        }
    }
    if (!isfinite(px)) {
        return false;
    }
    /* Optional /line-height. */
    while (p < e && *p == ' ') {
        p++;
    }
    if (p < e && *p == '/') {
        p++;
        while (p < e && *p == ' ') {
            p++;
        }
        const char *ls = p;
        while (p < e && *p != ' ') {
            p++;
        }
        if (p == ls) {
            return false;
        }
        double v;
        char unit[16];
        size_t ul;
        size_t used = scan_number(ls, (size_t)(p - ls), &v, unit, &ul);
        bool ok = (used == (size_t)(p - ls) && v >= 0) || (p - ls == 6 && SDL_strncasecmp(ls, "normal", 6) == 0);
        if (!ok) {
            return false;
        }
    }
    /* The families. */
    embi_sb fam = {0};
    embi_sb ser = {0};
    int nfam = 0;
    while (true) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            p++;
        }
        if (p >= e) {
            break;
        }
        embi_sb name = {0};
        bool quoted = false;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            quoted = true;
            while (p < e && *p != q) {
                if (*p == '\\' && p + 1 < e) {
                    p++;
                }
                embi_sb_append_char(&name, *p++);
            }
            if (p >= e) {
                embi_sb_free(&name);
                embi_sb_free(&fam);
                embi_sb_free(&ser);
                return false; /* unterminated string */
            }
            p++;
        } else {
            bool first = true;
            while (p < e && *p != ',') {
                const char *ws = p;
                while (p < e && *p != ',' && *p != ' ' && *p != '\t' && *p != '\n') {
                    p++;
                }
                if (p == ws) {
                    break;
                }
                if (!first) {
                    embi_sb_append_char(&name, ' ');
                }
                embi_sb_append(&name, (embi_str){ws, (uint32_t)(p - ws)});
                first = false;
                while (p < e && (*p == ' ' || *p == '\t' || *p == '\n')) {
                    p++;
                }
            }
        }
        while (p < e && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (name.len == 0 || (p < e && *p != ',')) {
            embi_sb_free(&name);
            embi_sb_free(&fam);
            embi_sb_free(&ser);
            return false;
        }
        if (!quoted) {
            /* Unquoted names are sequences of identifiers; the CSS-wide keywords and default are not family names, and
             * a generic family is serialized in lower case. */
            bool ok = true;
            for (uint32_t k = 0; k < name.len && ok; k++) {
                unsigned char ch = (unsigned char)name.data[k];
                bool word_start = k == 0 || name.data[k - 1] == ' ';
                if (ch == ' ') {
                    continue;
                }
                if (word_start && ch >= '0' && ch <= '9') {
                    ok = false;
                }
                ok = ok && (ch >= 0x80 || ch == '-' || ch == '_' || ch == '\\' || (ch >= '0' && ch <= '9') ||
                            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'));
            }
            static const char *const kw[] = {"initial", "inherit", "unset", "revert", "revert-layer", "default"};
            for (size_t k = 0; k < sizeof kw / sizeof *kw; k++) {
                if (strlen(kw[k]) == name.len && SDL_strncasecmp(name.data, kw[k], name.len) == 0) {
                    ok = false;
                }
            }
            if (!ok) {
                embi_sb_free(&name);
                embi_sb_free(&fam);
                embi_sb_free(&ser);
                return false;
            }
            if (is_generic(name.data, name.len)) {
                for (uint32_t k = 0; k < name.len; k++) {
                    if (name.data[k] >= 'A' && name.data[k] <= 'Z') {
                        name.data[k] = (char)(name.data[k] + 32);
                    }
                }
            }
        }
        if (nfam > 0) {
            embi_sb_append(&fam, EMBI_STR(","));
            embi_sb_append(&ser, EMBI_STR(", "));
        }
        embi_sb_append(&fam, (embi_str){name.data, name.len});
        if (quoted && !is_generic(name.data, name.len)) {
            embi_sb_append_char(&ser, '"');
            for (uint32_t k = 0; k < name.len; k++) {
                if (name.data[k] == '"' || name.data[k] == '\\') {
                    embi_sb_append_char(&ser, '\\');
                }
                embi_sb_append_char(&ser, name.data[k]);
            }
            embi_sb_append_char(&ser, '"');
        } else {
            embi_sb_append(&ser, (embi_str){name.data, name.len});
        }
        embi_sb_free(&name);
        nfam++;
        if (p < e && *p == ',') {
            p++;
            while (p < e && (*p == ' ' || *p == '\t')) {
                p++;
            }
            if (p >= e) {
                embi_sb_free(&fam);
                embi_sb_free(&ser);
                return false; /* trailing comma */
            }
        }
    }
    if (nfam == 0) {
        embi_sb_free(&fam);
        embi_sb_free(&ser);
        return false;
    }
    out->px = (float)px;
    out->family = dup_n(embi_sb_cstr(&fam), fam.len);
    /* The serialization: style, variant, weight, stretch, size, families. */
    embi_sb css_out = {0};
    if (out->style == 1) {
        embi_sb_append(&css_out, EMBI_STR("italic "));
    } else if (out->style == 2) {
        embi_sb_append(&css_out, EMBI_STR("oblique "));
    }
    if (out->small_caps) {
        embi_sb_append(&css_out, EMBI_STR("small-caps "));
    }
    if (out->weight != 400) {
        char wb[16];
        if (out->weight == 700) {
            SDL_snprintf(wb, sizeof wb, "bold ");
        } else {
            SDL_snprintf(wb, sizeof wb, "%d ", (int)out->weight);
        }
        embi_sb_append(&css_out, (embi_str){wb, (uint32_t)strlen(wb)});
    }
    if (out->stretch != 100) {
        for (int i = 0; i < 9; i++) {
            static const float pct[9] = {50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200};
            if (pct[i] == out->stretch) {
                embi_sb_append(&css_out, (embi_str){k_stretch[i], (uint32_t)strlen(k_stretch[i])});
                embi_sb_append_char(&css_out, ' ');
            }
        }
    }
    char szb[48];
    SDL_snprintf(szb, sizeof szb, "%gpx ", px);
    embi_sb_append(&css_out, (embi_str){szb, (uint32_t)strlen(szb)});
    embi_sb_append(&css_out, (embi_str){ser.data, ser.len});
    out->css = dup_n(embi_sb_cstr(&css_out), css_out.len);
    embi_sb_free(&css_out);
    embi_sb_free(&fam);
    embi_sb_free(&ser);
    return true;
}

bool embi_canvas_parse_color(embi_str css, uint32_t *rgba)
{
    uint8_t rgb[3];
    double a;
    if (!cs_parse_color(nullptr, css, rgb, &a)) {
        return false;
    }
    *rgba = (uint32_t)rgb[0] | ((uint32_t)rgb[1] << 8) | ((uint32_t)rgb[2] << 16) | ((uint32_t)(a * 255.0 + 0.5) << 24);
    return true;
}
