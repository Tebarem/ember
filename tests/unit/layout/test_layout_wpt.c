/*
 * The WPT check-layout runner (W3a). Each file in tests/wpt/layout-tests.txt is one test. The
 * runner reads the selectors of its checkLayout calls, lays out the page and checks the
 * data-expected-*, data-offset-*, data-total-* attributes of the matching elements and their
 * descendants, following tests/wpt/resources/check-layout-th.js. Scripts are not run.
 *
 * A file passes when every check passes and it has at least one check. Its outcome must match
 * the files in tests/layout/expectations exactly: a failing file must be listed there, and a listed
 * file must fail. Each line is "<WPT path> <reason> [note]"; reason is one of not-yet (note: the W3
 * task that fixes it), writing-mode, multicol, needs-js, font-metrics, out-of-scope, bug. The run
 * also fails when a listed path is not in layout-tests.txt, when a path is listed twice, and when
 * the bug entries of a directory exceed 3% of its tests.
 *
 * The fixtures of tests/layout/fixtures.txt (paths relative to tests/layout) run through the same
 * checks and must all pass; they have no expectations.
 *
 * Compare rule (check-layout-th.js assert_tolerance): the offset*, client*, scroll* values are
 * integers (rounded by the geometry API as CSSOM View does), and every value must differ from the
 * expected one by less than 1, which for integers is an exact compare.
 *
 * EMBER_LAYOUT_ONLY=<substring> prints the layout dump of the matching files.
 * EMBER_LAYOUT_FILTER=<substring> runs only the matching WPT files.
 * EMBER_LAYOUT_VERBOSE=1 prints the first mismatch of every failing file, expected or not.
 * EMBER_LAYOUT_REGEN=<file> writes one expectations line per failing WPT file (the maintenance
 * command that regenerates the expectations files from a full run).
 */
#include "emb_test.h"

#include "css/css.h"
#include "css/props.h"
#include "css/selector.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "layout/layout.h"
#include "layout/text_shim.h"
#include "style/style.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WPT_ROOT EMBER_SOURCE_DIR "/tests/wpt"
#define WPT_LIST EMBER_SOURCE_DIR "/tests/wpt/layout-tests.txt"
#define FIX_ROOT EMBER_SOURCE_DIR "/tests/layout"
#define FIX_LIST EMBER_SOURCE_DIR "/tests/layout/fixtures.txt"
#define WPT_EXPECT_DIR EMBER_SOURCE_DIR "/tests/layout/expectations"
#define WPT_PATH 1024
#define WPT_MAX_OWNED 512
#define WPT_MAX_SEL 16
#define WPT_MAX_SHEETS 64
#define WPT_SCROLLBAR 15.0f

/* Per-file state. Buffers loaded for the file are kept until the file's run ends, since the
 * engines may point into them. file is the HTML file, dir its directory. */
typedef struct wpt_ctx {
    void *owned[WPT_MAX_OWNED];
    uint32_t nowned;
    char file[WPT_PATH];
    char dir[WPT_PATH];
} wpt_ctx;

typedef struct wpt_res {
    uint32_t checks;
    uint32_t failures;
    char first[256];
    char reason[24]; /* why a failure is expected: a reason tag, from the file's content */
    char note[24];   /* the task for not-yet */
} wpt_res;

typedef struct wpt_expect {
    char path[WPT_PATH];
    char reason[24];
    char note[64];
    bool seen; /* the path is in the list of tests */
} wpt_expect;

static wpt_expect *expect_table;
static uint32_t expect_count, expect_cap;
static uint32_t expect_errors; /* malformed lines, duplicates, unknown reasons */

/* ------------------------------------------------------------------ files and URLs */

static void ctx_own(wpt_ctx *c, void *p)
{
    if (c->nowned < WPT_MAX_OWNED) {
        c->owned[c->nowned++] = p;
    } else {
        SDL_free(p);
    }
}

static void ctx_release(wpt_ctx *c)
{
    for (uint32_t i = 0; i < c->nowned; i++) {
        SDL_free(c->owned[i]);
    }
    c->nowned = 0;
}

/* The directory part of a path (without the trailing slash). */
static void dir_of(const char *path, char *out, size_t cap)
{
    SDL_snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash != nullptr) {
        *slash = '\0';
    }
}

/* The directory a base URL resolves against: the directory of a file path, or the test's own
 * directory when the base is empty. */
static void base_dir(const wpt_ctx *c, embi_str base, char *out, size_t cap)
{
    if (base.len == 0 || base.len >= WPT_PATH) {
        SDL_snprintf(out, cap, "%s", c->dir);
        return;
    }
    char tmp[WPT_PATH];
    memcpy(tmp, base.ptr, base.len);
    tmp[base.len] = '\0';
    dir_of(tmp, out, cap);
}

/* Joins a URL to a directory. Root-relative URLs resolve against tests/wpt. Query and fragment
 * parts are dropped. Returns false for URLs the runner does not load. */
static bool resolve_url(const char *dir, embi_str url, char *out, size_t cap)
{
    char tmp[WPT_PATH];
    if (url.len == 0 || url.len >= sizeof tmp) {
        return false;
    }
    memcpy(tmp, url.ptr, url.len);
    tmp[url.len] = '\0';
    char *cut = strpbrk(tmp, "?#");
    if (cut != nullptr) {
        *cut = '\0';
    }
    if (strncmp(tmp, "http:", 5) == 0 || strncmp(tmp, "https:", 6) == 0 || strncmp(tmp, "data:", 5) == 0) {
        return false;
    }
    if (tmp[0] == '/') {
        SDL_snprintf(out, cap, "%s%s", WPT_ROOT, tmp);
    } else {
        SDL_snprintf(out, cap, "%s/%s", dir, tmp);
    }
    return true;
}

static void *load_owned(wpt_ctx *c, const char *path, size_t *len)
{
    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    if (data != nullptr) {
        *len = n;
        ctx_own(c, data);
    }
    return data;
}

/* The stylesheet loader: fetches imports and linked sheets, relative to the importing sheet. */
static bool fetch_sheet(void *user, embi_str base, embi_str url, embi_str *resolved,
                        const unsigned char **bytes, size_t *n)
{
    wpt_ctx *c = user;
    char dir[WPT_PATH], path[WPT_PATH];
    base_dir(c, base, dir, sizeof dir);
    if (!resolve_url(dir, url, path, sizeof path)) {
        return false;
    }
    size_t len = 0;
    void *data = load_owned(c, path, &len);
    if (data == nullptr) {
        return false;
    }
    char *rp = SDL_strdup(path);
    if (rp == nullptr) {
        return false;
    }
    ctx_own(c, rp);
    *resolved = (embi_str){rp, (uint32_t)strlen(rp)};
    *bytes = data;
    *n = len;
    return true;
}

/* The font loader. The registry takes the buffer (SDL_free), so it is not kept here. */
static bool load_font(void *user, embi_str base, embi_str url, uint8_t **data, size_t *len)
{
    wpt_ctx *c = user;
    char dir[WPT_PATH], path[WPT_PATH];
    base_dir(c, base, dir, sizeof dir);
    if (!resolve_url(dir, url, path, sizeof path)) {
        return false;
    }
    size_t n = 0;
    void *buf = SDL_LoadFile(path, &n);
    if (buf == nullptr) {
        return false;
    }
    *data = buf;
    *len = n;
    return true;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* The natural size of an image from its header: PNG, GIF and JPEG. Other formats are not sized. */
static bool image_size_of(void *user, embi_str url, float *w, float *h, bool *has_ratio)
{
    wpt_ctx *c = user;
    char path[WPT_PATH];
    if (url.len > 19 && memcmp(url.ptr, "data:image/svg+xml,", 19) == 0) {
        /* An SVG image with no width, height or viewBox has no intrinsic size or ratio: it gets the
         * 300x150 default object size (CSS Images 3 section 5.3.3). Other SVG images are not sized. */
        char tmp[512];
        size_t m = url.len - 19 < sizeof tmp - 1 ? url.len - 19 : sizeof tmp - 1;
        memcpy(tmp, url.ptr + 19, m);
        tmp[m] = 0;
        if (strstr(tmp, "width") == nullptr && strstr(tmp, "height") == nullptr &&
            strstr(tmp, "viewBox") == nullptr) {
            *w = 300;
            *h = 150;
            *has_ratio = false;
            return true;
        }
        /* Both a width and a height attribute: that is the size, and the ratio follows from it. */
        const char *pw = strstr(tmp, "width='");
        const char *ph = strstr(tmp, "height='");
        if (pw != nullptr && ph != nullptr) {
            *w = (float)SDL_atoi(pw + 7);
            *h = (float)SDL_atoi(ph + 8);
            *has_ratio = *w > 0 && *h > 0;
            return *has_ratio;
        }
        return false;
    }
    if (!resolve_url(c->dir, url, path, sizeof path)) {
        return false;
    }
    size_t n = 0;
    uint8_t *d = SDL_LoadFile(path, &n);
    bool ok = false;
    if (d == nullptr || n < 16) {
        SDL_free(d);
        return false;
    }
    if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
        *w = (float)be32(d + 16);
        *h = (float)be32(d + 20);
        ok = true;
    } else if (memcmp(d, "GIF8", 4) == 0) {
        *w = (float)(d[6] | (d[7] << 8));
        *h = (float)(d[8] | (d[9] << 8));
        ok = true;
    } else if (d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) {
                i++;
                continue;
            }
            uint8_t m = d[i + 1];
            if (m >= 0xC0 && m <= 0xC2) {
                *h = (float)((d[i + 5] << 8) | d[i + 6]);
                *w = (float)((d[i + 7] << 8) | d[i + 8]);
                ok = true;
                break;
            }
            size_t seg = (size_t)((d[i + 2] << 8) | d[i + 3]);
            i += 2 + seg;
        }
    }
    SDL_free(d);
    if (ok) {
        *has_ratio = *w > 0 && *h > 0;
    }
    return ok;
}

/* ------------------------------------------------------------------ attribute helpers */

static bool attr_text(const embi_element *el, const char *name, char *out, size_t cap)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)el, (embi_str){name, (uint32_t)strlen(name)});
    if (a == nullptr || a->value.wide || a->value.len == 0 || a->value.len >= cap) {
        return false;
    }
    memcpy(out, a->value.ptr, a->value.len);
    out[a->value.len] = '\0';
    return true;
}

/* Parses a JS-Number-like value. False when it is not a number. */
static bool parse_num(const char *s, double *out)
{
    char *end = nullptr;
    double v = strtod(s, &end);
    if (end == s) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* assert_tolerance from check-layout-th.js: passes when the expected value is a number and the
 * actual value differs from it by less than 1. */
static bool within_one(double actual, const char *expected)
{
    double e = 0;
    if (!parse_num(expected, &e)) {
        return false;
    }
    return fabs(actual - e) < 1.0;
}

/* ------------------------------------------------------------------ element checks */

static const embi_element *g_cur_el;

static void fail_check(wpt_res *r, const char *what, const char *expected, double actual)
{
    r->failures++;
    if (SDL_getenv("EMBER_LAYOUT_ALL") != nullptr && g_cur_el != nullptr) {
        char cls[64] = "";
        (void)attr_text(g_cur_el, "class", cls, sizeof cls);
        char id[64] = "";
        (void)attr_text(g_cur_el, "id", id, sizeof id);
        char sty[96] = "";
        (void)attr_text(g_cur_el, "style", sty, sizeof sty);
        embi_str tn = embi_atom_str(g_cur_el->base.name);
        SDL_Log("  mismatch <%.*s id=%s class=%s style=%s> %s: expected %s, got %.3f", (int)tn.len, tn.ptr, id, cls,
                sty, what, expected, actual);
    }
    if (r->first[0] == '\0') {
        SDL_snprintf(r->first, sizeof r->first, "%s: expected %s, got %.3f", what, expected, actual);
    }
}

static void check_element(embi_layout_engine *eng, embi_style_engine *style, const embi_element *el,
                          wpt_res *r)
{
    char v[64];
    g_cur_el = el;
    float x = 0, y = 0, w = 0, h = 0;
    const embi_element *parent = nullptr;
    (void)embi_layout_offset(eng, el, &parent, &x, &y, &w, &h);
    float bdr[4] = {0, 0, 0, 0}, pad[4] = {0, 0, 0, 0}, mg[4] = {0, 0, 0, 0};
    (void)embi_layout_box_edges(eng, el, bdr, pad, mg);
    float cw = 0, ch = 0, sw = 0, sh = 0;
    (void)embi_layout_client_size(eng, el, &cw, &ch);
    (void)embi_layout_scroll_size(eng, el, &sw, &sh);
    embi_rectf br = {0, 0, 0, 0};
    (void)embi_layout_border_box(eng, el, &br);

    if (attr_text(el, "data-expected-width", v, sizeof v)) {
        r->checks++;
        if (!within_one(w, v)) {
            fail_check(r, "offsetWidth", v, w);
        }
    }
    if (attr_text(el, "data-expected-height", v, sizeof v)) {
        r->checks++;
        if (!within_one(h, v)) {
            fail_check(r, "offsetHeight", v, h);
        }
    }
    if (attr_text(el, "data-offset-x", v, sizeof v)) {
        r->checks++;
        if (!within_one(x, v)) {
            fail_check(r, "offsetLeft", v, x);
        }
    }
    if (attr_text(el, "data-offset-y", v, sizeof v)) {
        r->checks++;
        if (!within_one(y, v)) {
            fail_check(r, "offsetTop", v, y);
        }
    }
    if (attr_text(el, "data-expected-client-width", v, sizeof v)) {
        r->checks++;
        if (!within_one(cw, v)) {
            fail_check(r, "clientWidth", v, cw);
        }
    }
    if (attr_text(el, "data-expected-client-height", v, sizeof v)) {
        r->checks++;
        if (!within_one(ch, v)) {
            fail_check(r, "clientHeight", v, ch);
        }
    }
    if (attr_text(el, "data-expected-scroll-width", v, sizeof v)) {
        r->checks++;
        if (!within_one(sw, v)) {
            fail_check(r, "scrollWidth", v, sw);
        }
    }
    if (attr_text(el, "data-expected-scroll-height", v, sizeof v)) {
        r->checks++;
        if (!within_one(sh, v)) {
            fail_check(r, "scrollHeight", v, sh);
        }
    }
    if (attr_text(el, "data-expected-bounding-client-rect-width", v, sizeof v)) {
        r->checks++;
        if (!within_one(br.w, v)) {
            fail_check(r, "getBoundingClientRect().width", v, br.w);
        }
    }
    if (attr_text(el, "data-expected-bounding-client-rect-height", v, sizeof v)) {
        r->checks++;
        if (!within_one(br.h, v)) {
            fail_check(r, "getBoundingClientRect().height", v, br.h);
        }
    }
    if (attr_text(el, "data-total-x", v, sizeof v)) {
        r->checks++;
        if (!within_one(SDL_roundf(bdr[0]) + x, v)) {
            fail_check(r, "clientLeft+offsetLeft", v, SDL_roundf(bdr[0]) + x);
        }
    }
    if (attr_text(el, "data-total-y", v, sizeof v)) {
        r->checks++;
        if (!within_one(SDL_roundf(bdr[1]) + y, v)) {
            fail_check(r, "clientTop+offsetTop", v, SDL_roundf(bdr[1]) + y);
        }
    }

    const embi_style *s = embi_style_get(style, (embi_element *)el, EMBI_PE_NONE);
    if (attr_text(el, "data-expected-display", v, sizeof v)) {
        r->checks++;
        embi_sb sb = {nullptr, 0, 0};
        bool ok = s != nullptr && embi_style_value_text(s, embi_prop_lookup(EMBI_STR("display")), &sb);
        bool same = ok && sb.len == strlen(v) && memcmp(sb.data, v, sb.len) == 0;
        if (!same) {
            r->failures++;
            if (r->first[0] == '\0') {
                SDL_snprintf(r->first, sizeof r->first, "display: expected %s", v);
            }
        }
        embi_sb_free(&sb);
    }

    /* Paddings and margins are the used values kept on the element's fragment. */
    static const char *const pad_attrs[4] = {"data-expected-padding-left", "data-expected-padding-top",
                                             "data-expected-padding-right", "data-expected-padding-bottom"};
    static const char *const margin_attrs[4] = {"data-expected-margin-left", "data-expected-margin-top",
                                                "data-expected-margin-right", "data-expected-margin-bottom"};
    for (int i = 0; i < 4; i++) {
        if (attr_text(el, pad_attrs[i], v, sizeof v)) {
            r->checks++;
            if (!within_one(pad[i], v)) {
                fail_check(r, pad_attrs[i], v, pad[i]);
            }
        }
        if (attr_text(el, margin_attrs[i], v, sizeof v)) {
            r->checks++;
            if (!within_one(mg[i], v)) {
                fail_check(r, margin_attrs[i], v, mg[i]);
            }
        }
    }
}

/* ------------------------------------------------------------------ the checkLayout selectors */

/* Reads the checkLayout selectors of a file in order. Returns the count. */
static uint32_t read_selectors(const char *html, size_t n, char sel[][256], uint32_t cap)
{
    uint32_t count = 0;
    const char *p = html;
    const char *end = html + n;
    while (p < end && count < cap) {
        const char *hit = SDL_strstr(p, "checkLayout(");
        if (hit == nullptr || hit >= end) {
            break;
        }
        p = hit + SDL_strlen("checkLayout(");
        while (p < end && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (p >= end || (*p != '\'' && *p != '"')) {
            continue;
        }
        char q = *p++;
        const char *close = p;
        while (close < end && *close != q && *close != '\n') {
            close++;
        }
        if (close >= end || *close != q) {
            continue;
        }
        size_t len = (size_t)(close - p);
        if (len > 0 && len < 256) {
            memcpy(sel[count], p, len);
            sel[count][len] = '\0';
            count++;
        }
        p = close + 1;
    }
    return count;
}

/* The <style> and linked stylesheets of the raw HTML, in document order. */
static uint32_t collect_sheets(wpt_ctx *c, const char *html, size_t n, embi_css_sheet **out, uint32_t cap)
{
    embi_css_hooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.decl_ok = embi_prop_decl_ok;
    embi_css_loader loader = {c, fetch_sheet};
    uint32_t count = 0;
    const char *p = html;
    const char *end = html + n;
    while (p < end && count < cap) {
        const char *st = SDL_strcasestr(p, "<style");
        const char *ln = SDL_strcasestr(p, "<link");
        const char *tag = st;
        bool is_style = true;
        if (ln != nullptr && (st == nullptr || ln < st)) {
            tag = ln;
            is_style = false;
        }
        if (tag == nullptr || tag >= end) {
            break;
        }
        const char *gt = SDL_strchr(tag, '>');
        if (gt == nullptr) {
            break;
        }
        if (is_style) {
            const char *close = SDL_strcasestr(gt + 1, "</style>");
            size_t blen = close != nullptr ? (size_t)(close - (gt + 1)) : (size_t)(end - (gt + 1));
            embi_css_parse_opts opts = {&loader, &hooks, (embi_str){c->file, (uint32_t)strlen(c->file)}};
            embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){gt + 1, (uint32_t)blen}, &opts);
            if (sheet != nullptr) {
                out[count++] = sheet;
            }
            p = close != nullptr ? close + 8 : end;
            continue;
        }
        size_t tl = (size_t)(gt - tag);
        char tagbuf[1024];
        p = gt + 1;
        if (tl >= sizeof tagbuf) {
            continue;
        }
        memcpy(tagbuf, tag, tl);
        tagbuf[tl] = '\0';
        const char *rel = SDL_strcasestr(tagbuf, "stylesheet");
        const char *href = SDL_strcasestr(tagbuf, "href=");
        if (rel == nullptr || href == nullptr) {
            continue;
        }
        href += 5;
        char q = (*href == '"' || *href == '\'') ? *href++ : ' ';
        const char *he = href;
        while (*he != '\0' && *he != q && *he != ' ' && *he != '>') {
            he++;
        }
        embi_str url = {href, (uint32_t)(he - href)};
        embi_str resolved = {nullptr, 0};
        const unsigned char *bytes = nullptr;
        size_t blen = 0;
        embi_str base = {c->file, (uint32_t)strlen(c->file)};
        if (!fetch_sheet(c, base, url, &resolved, &bytes, &blen)) {
            continue;
        }
        embi_css_parse_opts opts = {&loader, &hooks, resolved};
        embi_css_sheet *sheet = embi_css_sheet_parse((embi_str){(const char *)bytes, (uint32_t)blen}, &opts);
        if (sheet != nullptr) {
            out[count++] = sheet;
        }
    }
    return count;
}

/* True when the property name occurs in the text as a declaration ("name:" not preceded by a
 * letter or hyphen, so "columns" does not match "grid-template-columns"). */
static bool has_prop(const char *html, size_t n, const char *name)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < n; i++) {
        if (html[i] != name[0] || memcmp(html + i, name, nl) != 0) {
            continue;
        }
        char before = i > 0 ? html[i - 1] : ' ';
        if ((before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') || before == '-') {
            continue;
        }
        size_t j = i + nl;
        while (j < n && (html[j] == ' ' || html[j] == '\t')) {
            j++;
        }
        if (j < n && html[j] == ':') {
            return true;
        }
    }
    return false;
}

/* True when the text has a writing-mode declaration other than horizontal-tb. */
static bool has_vertical_writing(const char *html, size_t n)
{
    for (size_t i = 0; i + 12 < n; i++) {
        if (memcmp(html + i, "writing-mode", 12) != 0) {
            continue;
        }
        size_t j = i + 12;
        while (j < n && (html[j] == ' ' || html[j] == ':')) {
            j++;
        }
        if (j + 10 <= n && memcmp(html + j, "horizontal", 10) != 0 && html[j] != '"' && html[j] != '\'') {
            return true;
        }
    }
    return false;
}

/* Why a failing file is expected to fail now: a reason tag from what the file needs (out of scope
 * for W3: vertical writing, multicol, grid-lanes, anchors, field-sizing), else not-yet and the
 * task that implements the formatting context it tests, by directory. */
static void classify(const char *rel, const char *html, size_t n, wpt_res *r)
{
    const char *task = "W3g";
    if (strstr(rel, "css/css-flexbox/") != nullptr) {
        task = "W3e";
    } else if (strstr(rel, "css/css-grid/") != nullptr) {
        task = "W3f";
    } else if (strstr(rel, "css/css-tables/") != nullptr) {
        task = "W3g";
    } else if (strstr(rel, "css/css-position/") != nullptr || strstr(rel, "/positioning/") != nullptr) {
        task = "W3d";
    } else if (strstr(rel, "/floats/") != nullptr) {
        task = "W3b";
    } else if (strstr(rel, "css/CSS2/") != nullptr) {
        task = "W3c";
    }
    if (strcmp(task, "W3c") == 0 && has_prop(html, n, "float")) {
        task = "W3b";
    }
    SDL_snprintf(r->note, sizeof r->note, "%s", task);
    SDL_snprintf(r->reason, sizeof r->reason, "not-yet");
    if (strstr(rel, "grid-lanes") != nullptr || strstr(rel, "masonry") != nullptr) {
        SDL_snprintf(r->reason, sizeof r->reason, "out-of-scope");
        SDL_snprintf(r->note, sizeof r->note, "grid-lanes");
    } else if (has_vertical_writing(html, n)) {
        SDL_snprintf(r->reason, sizeof r->reason, "writing-mode");
        SDL_snprintf(r->note, sizeof r->note, "vertical");
    } else if (has_prop(html, n, "column-count") || has_prop(html, n, "columns") ||
               has_prop(html, n, "column-width")) {
        SDL_snprintf(r->reason, sizeof r->reason, "multicol");
        SDL_snprintf(r->note, sizeof r->note, "columns");
    } else if (has_prop(html, n, "field-sizing")) {
        SDL_snprintf(r->reason, sizeof r->reason, "out-of-scope");
        SDL_snprintf(r->note, sizeof r->note, "field-sizing");
    } else if (has_prop(html, n, "anchor-name") || has_prop(html, n, "position-anchor")) {
        SDL_snprintf(r->reason, sizeof r->reason, "out-of-scope");
        SDL_snprintf(r->note, sizeof r->note, "anchor");
    }
}

/* ------------------------------------------------------------------ one WPT file */

/* Lays out one WPT file and checks it. The return value is false only when the file cannot be
 * read; a file that fails its checks still returns true with r->failures set. */
static bool run_file(const char *base, const char *rel, wpt_res *r)
{
    memset(r, 0, sizeof *r);
    char path[WPT_PATH];
    SDL_snprintf(path, sizeof path, "%s/%s", base, rel);
    size_t hlen = 0;
    char *html = SDL_LoadFile(path, &hlen);
    if (html == nullptr) {
        r->failures++;
        SDL_snprintf(r->first, sizeof r->first, "cannot read the file");
        return false;
    }

    char sel[WPT_MAX_SEL][256];
    uint32_t nsel = read_selectors(html, hlen, sel, WPT_MAX_SEL);
    if (nsel == 0) {
        r->failures++;
        SDL_snprintf(r->first, sizeof r->first, "no checkLayout call");
        SDL_free(html);
        return true;
    }

    wpt_ctx *c = SDL_calloc(1, sizeof *c);
    SDL_snprintf(c->file, sizeof c->file, "%s", path);
    dir_of(path, c->dir, sizeof c->dir);

    embi_text *text = embi_tshim_create();
    embi_style_callbacks cb = {c, load_font};
    embi_style_engine *style = embi_style_engine_create(embi_tshim_fonts(text), nullptr, &cb);
    embi_document *doc = embi_document_new(true);
    embi_html_parser *parser = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(parser, html, hlen);
    embi_html_parser_end(parser);
    embi_html_parser_destroy(parser);

    embi_css_sheet *sheets[WPT_MAX_SHEETS];
    uint32_t nsheets = collect_sheets(c, html, hlen, sheets, WPT_MAX_SHEETS);
    embi_document_set_author_sheets(style, doc, sheets, nsheets);
    embi_style_resolve_document(style, doc);

    embi_layout_config cfg = {c, image_size_of, WPT_SCROLLBAR};
    embi_layout_engine *eng = embi_layout_engine_create(text, style, &cfg);
    (void)embi_layout_document(eng, doc);

    const char *only = SDL_getenv("EMBER_LAYOUT_ONLY");
    if (only != nullptr && only[0] != '\0' && strstr(rel, only) != nullptr) {
        embi_sb sb = {nullptr, 0, 0};
        embi_layout_dump(eng, doc, &sb);
        SDL_Log("layout dump of %s:\n%.*s", rel, (int)sb.len, sb.data != nullptr ? sb.data : "");
        embi_sb_free(&sb);
    }

    embi_element *root = embi_document_element(doc);
    for (uint32_t s = 0; s < nsel; s++) {
        embi_arena a;
        embi_arena_init(&a, 1024);
        const embi_sel_list *list = embi_sel_parse_text(&a, (embi_str){sel[s], (uint32_t)strlen(sel[s])}, nullptr);
        if (list == nullptr) {
            r->failures++;
            if (r->first[0] == '\0') {
                SDL_snprintf(r->first, sizeof r->first, "bad selector %s", sel[s]);
            }
            embi_arena_free(&a);
            continue;
        }
        for (embi_node *n = (embi_node *)root; n != nullptr; n = embi_next_in_tree((embi_node *)root, n)) {
            if (!embi_is_element(n)) {
                continue;
            }
            embi_element *el = embi_as_element(n);
            embi_sel_ctx ctx;
            embi_sel_ctx_init(&ctx, nullptr, nullptr);
            bool hit = embi_sel_match_list(list, el, &ctx);
            embi_sel_ctx_free(&ctx);
            if (!hit) {
                continue;
            }
            /* The reference checks the matched node's parent, then the node and its subtree. */
            embi_node *up = el->base.parent;
            if (up != nullptr && embi_is_element(up)) {
                check_element(eng, style, embi_as_element(up), r);
            }
            for (embi_node *d = (embi_node *)el; d != nullptr; d = embi_next_in_tree((embi_node *)el, d)) {
                if (embi_is_element(d)) {
                    check_element(eng, style, embi_as_element(d), r);
                }
            }
        }
        embi_arena_free(&a);
    }
    if (r->checks == 0 && r->failures == 0) {
        r->failures++;
        SDL_snprintf(r->first, sizeof r->first, "no data-* attributes matched");
    }

    classify(rel, html, hlen, r);
    embi_layout_engine_destroy(eng);
    embi_style_forget_document(style, doc);
    embi_document_free(doc);
    for (uint32_t i = 0; i < nsheets; i++) {
        embi_css_sheet_unref(sheets[i]);
    }
    embi_style_engine_destroy(style);
    embi_tshim_destroy(text);
    ctx_release(c);
    SDL_free(c);
    SDL_free(html);
    return true;
}

/* ------------------------------------------------------------------ expectations */

static bool reason_ok(const char *reason)
{
    static const char *const known[] = {"not-yet", "writing-mode", "multicol", "needs-js",
                                        "font-metrics", "out-of-scope", "bug"};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        if (strcmp(reason, known[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_task(const char *note)
{
    return strlen(note) == 3 && note[0] == 'W' && note[1] == '3' && note[2] >= 'b' && note[2] <= 'i';
}

static void add_expect(const char *path, const char *reason, const char *note)
{
    for (uint32_t i = 0; i < expect_count; i++) {
        if (strcmp(expect_table[i].path, path) == 0) {
            SDL_Log("expectations: %s is listed twice", path);
            expect_errors++;
            return;
        }
    }
    if (expect_count == expect_cap) {
        uint32_t cap = expect_cap != 0 ? expect_cap * 2 : 256;
        wpt_expect *t = SDL_realloc(expect_table, cap * sizeof *t);
        if (t == nullptr) {
            expect_errors++;
            return;
        }
        expect_table = t;
        expect_cap = cap;
    }
    wpt_expect *e = &expect_table[expect_count++];
    memset(e, 0, sizeof *e);
    SDL_snprintf(e->path, sizeof e->path, "%s", path);
    SDL_snprintf(e->reason, sizeof e->reason, "%s", reason);
    SDL_snprintf(e->note, sizeof e->note, "%s", note);
}

/* Reads one expectations file: lines "<path> <reason> [note]". Comments start with #. */
static void parse_expect_file(const char *file)
{
    size_t n = 0;
    char *buf = SDL_LoadFile(file, &n);
    if (buf == nullptr) {
        expect_errors++;
        return;
    }
    /* SDL_LoadFile appends a NUL, so the last line is terminated. */
    char *p = buf;
    char *end = buf + n;
    while (p < end) {
        char *eol = p;
        while (eol < end && *eol != '\n') {
            eol++;
        }
        if (eol < end) {
            *eol = '\0';
        }
        if (p[0] != '#' && p[0] != '\0' && p[0] != '\r') {
            char path[WPT_PATH], reason[24], note[64];
            note[0] = '\0';
            int got = SDL_sscanf(p, "%1023s %23s %63s", path, reason, note);
            if (got < 2 || !reason_ok(reason) || (strcmp(reason, "not-yet") == 0 && !is_task(note))) {
                SDL_Log("expectations: %s: bad line: %s", file, p);
                expect_errors++;
            } else {
                add_expect(path, reason, note);
            }
        }
        p = eol + 1;
    }
    SDL_free(buf);
}

static SDL_EnumerationResult on_expect_file(void *ud, const char *dirname, const char *fname)
{
    (void)ud;
    if (fname[0] == '.') {
        return SDL_ENUM_CONTINUE;
    }
    char path[WPT_PATH];
    SDL_snprintf(path, sizeof path, "%s/%s", dirname, fname);
    parse_expect_file(path);
    return SDL_ENUM_CONTINUE;
}

static wpt_expect *find_expect(const char *rel)
{
    for (uint32_t i = 0; i < expect_count; i++) {
        if (strcmp(expect_table[i].path, rel) == 0) {
            return &expect_table[i];
        }
    }
    return nullptr;
}

/* The group of a path for the summary and the bug cap: its WPT spec directory (css/css-flexbox,
 * css/CSS2, ...), the first two path components. */
static void group_of(const char *rel, char *out, size_t cap)
{
    size_t n = 0, slashes = 0;
    while (rel[n] != '\0') {
        if (rel[n] == '/' && ++slashes == 2) {
            break;
        }
        n++;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, rel, n);
    out[n] = '\0';
}

typedef struct wpt_group {
    char name[96];
    uint32_t total, pass, bugs;
} wpt_group;

/* Calls fn(root, rel, user) for every non-comment line of a list file. */
typedef void (*list_fn)(const char *rel, void *user);

static bool for_each_line(const char *file, list_fn fn, void *user)
{
    size_t n = 0;
    char *list = SDL_LoadFile(file, &n);
    if (list == nullptr) {
        return false;
    }
    char *p = list;
    char *end = list + n;
    while (p < end) {
        char *eol = p;
        while (eol < end && *eol != '\n') {
            eol++;
        }
        size_t rl = (size_t)(eol - p);
        if (rl > 0 && p[rl - 1] == '\r') {
            rl--;
        }
        char rel[WPT_PATH];
        if (rl > 0 && rl < sizeof rel && p[0] != '#') {
            memcpy(rel, p, rl);
            rel[rl] = '\0';
            fn(rel, user);
        }
        p = eol + 1;
    }
    SDL_free(list);
    return true;
}

/* ------------------------------------------------------------------ the tests */

typedef struct wpt_run {
    wpt_group groups[16];
    uint32_t ngroups;
    uint32_t files, passed, unexpected_fail, unexpected_pass, expected_fail, checks, feature_off;
    bool verbose;
    bool regen;
    embi_sb regen_out;
} wpt_run;

static void wpt_one(const char *rel, void *user)
{
    wpt_run *w = user;
    wpt_res r;
    const char *flt = SDL_getenv("EMBER_LAYOUT_FILTER");
    if (flt != nullptr && flt[0] != 0 && strstr(rel, flt) == nullptr) {
        return;
    }
    (void)run_file(WPT_ROOT, rel, &r);
    w->files++;
    w->checks += r.checks;
    char gname[96];
    group_of(rel, gname, sizeof gname);
    wpt_group *g = nullptr;
    for (uint32_t i = 0; i < w->ngroups; i++) {
        if (strcmp(w->groups[i].name, gname) == 0) {
            g = &w->groups[i];
        }
    }
    if (g == nullptr && w->ngroups < 16) {
        g = &w->groups[w->ngroups++];
        memset(g, 0, sizeof *g);
        SDL_snprintf(g->name, sizeof g->name, "%s", gname);
    }
    if (g == nullptr) {
        SDL_Log("too many WPT groups");
        w->unexpected_fail++;
        return;
    }
    g->total++;
    wpt_expect *ex = find_expect(rel);
    if (ex != nullptr) {
        ex->seen = true;
        if (strcmp(ex->reason, "bug") == 0) {
            g->bugs++;
        }
    }
    if (r.failures == 0) {
        g->pass++;
        w->passed++;
        if (ex != nullptr) {
            w->unexpected_pass++;
            SDL_Log("UNEXPECTED PASS (remove from expectations): %s", rel);
        }
        return;
    }
    if (ex != nullptr) {
        w->expected_fail++;
        if (w->verbose) {
            SDL_Log("expected failure %s [%s]: %s", rel, ex->reason, r.first);
        }
    } else {
        char full[WPT_PATH + 64];
        SDL_snprintf(full, sizeof full, "%s/%s", WPT_ROOT, rel);
        if (emb_test_source_needs_disabled_feature(full)) {
            w->feature_off++; /* SZ3b: the page uses SVG or canvas and this build has it off */
        } else {
            w->unexpected_fail++;
            SDL_Log("FAIL %s: %s (%u of %u checks failed)", rel, r.first, r.failures, r.checks);
        }
    }
    if (w->regen) {
        char line[WPT_PATH + 64];
        SDL_snprintf(line, sizeof line, "%s %s %s\n", rel, r.reason, r.note);
        embi_sb_append(&w->regen_out, (embi_str){line, (uint32_t)strlen(line)});
    }
}

EMB_TEST(layout_wpt_check_layout_matches_expectations)
{
    expect_count = 0;
    expect_errors = 0;
    EMB_REQUIRE(SDL_EnumerateDirectory(WPT_EXPECT_DIR, on_expect_file, nullptr));
    EMB_CHECK_EQ_INT(expect_errors, 0);

    wpt_run w;
    memset(&w, 0, sizeof w);
    w.verbose = SDL_getenv("EMBER_LAYOUT_VERBOSE") != nullptr;
    const char *regen_path = SDL_getenv("EMBER_LAYOUT_REGEN");
    w.regen = regen_path != nullptr;
    EMB_REQUIRE(for_each_line(WPT_LIST, wpt_one, &w));

    for (uint32_t i = 0; i < w.ngroups; i++) {
        SDL_Log("%-28s %u / %u pass (%u%%)", w.groups[i].name, w.groups[i].pass, w.groups[i].total,
                w.groups[i].total != 0 ? w.groups[i].pass * 100 / w.groups[i].total : 0);
        /* bug entries are capped at 3% of the directory */
        EMB_CHECK(w.groups[i].bugs * 100 <= w.groups[i].total * 3);
    }
    uint32_t unknown = 0;
    for (uint32_t i = 0; i < expect_count; i++) {
        if (!expect_table[i].seen) {
            unknown++;
            SDL_Log("expectations: %s is not in layout-tests.txt", expect_table[i].path);
        }
    }
    if (w.regen && w.regen_out.data != nullptr) {
        SDL_IOStream *io = SDL_IOFromFile(regen_path, "wb");
        if (io != nullptr) {
            SDL_WriteIO(io, w.regen_out.data, w.regen_out.len);
            SDL_CloseIO(io);
        }
    }
    embi_sb_free(&w.regen_out);
    SDL_free(expect_table);
    expect_table = nullptr;
    expect_count = expect_cap = 0;
    SDL_Log("WPT layout: %u files, %u passed, %u expected failures, %u checks", w.files, w.passed,
            w.expected_fail, w.checks);
    EMB_CHECK(w.files > 0);
    EMB_CHECK_EQ_INT(unknown, 0);
    EMB_CHECK_EQ_INT(w.unexpected_fail, 0);
    EMB_CHECK_EQ_INT(w.unexpected_pass, 0);
}

typedef struct fix_run {
    uint32_t files, failed, checks;
} fix_run;

static void fix_one(const char *rel, void *user)
{
    fix_run *f = user;
    wpt_res r;
    bool ok = run_file(FIX_ROOT, rel, &r);
    f->files++;
    f->checks += r.checks;
    if (!ok || r.failures != 0) {
        f->failed++;
        SDL_Log("FIXTURE FAIL %s: %s (%u of %u checks failed)", rel, r.first, r.failures, r.checks);
    }
}

/* Our own fixtures: every one must pass, with no expectations. */
EMB_TEST(layout_fixtures_pass)
{
    fix_run f;
    memset(&f, 0, sizeof f);
    EMB_REQUIRE(for_each_line(FIX_LIST, fix_one, &f));
    SDL_Log("layout fixtures: %u files, %u checks, %u failed", f.files, f.checks, f.failed);
    EMB_CHECK(f.files > 0);
    EMB_CHECK_EQ_INT(f.failed, 0);
}

EMB_TEST_MAIN()
