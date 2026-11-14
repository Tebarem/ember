/*
 * Container handling (sfnt, TTC), table directory, head/hhea/maxp/hmtx/OS2/post/name, cmap
 * lookups and the fvar axis and instance metadata.
 */
#include "font/font_internal.h"

#include <SDL3/SDL_stdinc.h>

#define EMBI_FONT_MAX_AXES 64u
#define EMBI_FONT_MAX_INSTANCES 4096u

/* ------------------------------------------------------------------ table directory */

typedef struct font_dir {
    embi_bytes file;
    uint32_t base;  /* offset of the sfnt header of the selected face */
    uint32_t ntab; /* number of table records */
} font_dir;

/* Table lookup result: absent from the directory, present and inside the file, or listed but
 * extending past the end of the file (broken). */
enum { TBL_ABSENT = 0, TBL_OK = 1, TBL_BROKEN = 2 };

/* Finds a table by tag. Bounds are checked lazily, per table, so an out-of-file record of a table
 * nobody asks for (DSIG, a vendor table) never fails the font. */
static int font_table_ex(const font_dir *d, uint32_t tag, embi_bytes *out)
{
    *out = (embi_bytes){nullptr, 0};
    for (uint32_t i = 0; i < d->ntab; i++) {
        uint32_t t = 0, off = 0, len = 0;
        uint64_t rec = (uint64_t)d->base + 12u + 16u * (uint64_t)i;
        if (!bt_u32(d->file, rec, &t) || !bt_u32(d->file, rec + 8, &off) ||
            !bt_u32(d->file, rec + 12, &len)) {
            return TBL_ABSENT;
        }
        if (t == tag) {
            return bt_sub(d->file, off, len, out) ? TBL_OK : TBL_BROKEN;
        }
    }
    return TBL_ABSENT;
}

/* Present and inside the file; a broken table counts as absent. */
static bool font_table(const font_dir *d, uint32_t tag, embi_bytes *out)
{
    return font_table_ex(d, tag, out) == TBL_OK;
}

/* Reads the record count. The records themselves are validated lazily by font_table_ex; a
 * directory cut short by the end of the file just loses the records that do not fit. */
static bool font_dir_check(font_dir *d)
{
    uint32_t ntab = 0;
    if (!bt_u16(d->file, (uint64_t)d->base + 4, &ntab)) {
        return false;
    }
    if (ntab == 0 || ntab > 1000) {
        return false;
    }
    uint64_t start = (uint64_t)d->base + 12u;
    if (start > d->file.n) {
        return false;
    }
    uint64_t fit = (d->file.n - start) / 16u;
    if (fit == 0) {
        return false;
    }
    d->ntab = ntab < fit ? ntab : (uint32_t)fit;
    return true;
}

/* ------------------------------------------------------------------ container */

uint32_t embi_font_count(const uint8_t *data, size_t len)
{
    embi_bytes file = {data, len};
    uint32_t sig = 0;
    if (data == nullptr || !bt_u32(file, 0, &sig)) {
        return 0;
    }
    if (sig == EMBI_TAG('t', 't', 'c', 'f')) {
        uint32_t n = 0;
        if (!bt_u32(file, 8, &n) || n == 0 || n > 1024) {
            return 0;
        }
        return n;
    }
    if (sig == 0x00010000u || sig == EMBI_TAG('t', 'r', 'u', 'e') ||
        sig == EMBI_TAG('O', 'T', 'T', 'O')) {
        return 1;
    }
    return 0;
}

/* Maps a face index to the offset of its sfnt header. */
static bool font_face_base(embi_bytes file, uint32_t index, uint32_t *base)
{
    uint32_t sig = 0;
    if (!bt_u32(file, 0, &sig)) {
        return false;
    }
    if (sig == EMBI_TAG('t', 't', 'c', 'f')) {
        uint32_t n = 0;
        if (!bt_u32(file, 8, &n) || index >= n || n > 1024) {
            return false;
        }
        uint32_t off = 0;
        if (!bt_u32(file, 12 + 4ull * index, &off)) {
            return false;
        }
        *base = off;
        return true;
    }
    if (index != 0) {
        return false;
    }
    *base = 0;
    return true;
}

/* ------------------------------------------------------------------ names */

typedef struct name_pick {
    uint32_t off, len; /* into the name table */
    uint32_t kind;     /* 0 none, 1 Mac Roman (8-bit), 2 UTF-16BE */
    uint32_t score;
} name_pick;

/* Scores the record for nameID: Windows en-US first, then other UTF-16 entries, then Mac. */
static void name_consider(embi_bytes nt, uint32_t rec_off, uint32_t want, name_pick *best)
{
    uint32_t pid = 0, eid = 0, lang = 0, nid = 0, len = 0, off = 0;
    if (!bt_u16(nt, rec_off, &pid) || !bt_u16(nt, rec_off + 2, &eid) ||
        !bt_u16(nt, rec_off + 4, &lang) || !bt_u16(nt, rec_off + 6, &nid) ||
        !bt_u16(nt, rec_off + 8, &len) || !bt_u16(nt, rec_off + 10, &off)) {
        return;
    }
    if (nid != want) {
        return;
    }
    /* A record whose string lies outside the table is skipped before scoring, so it cannot hide
     * a valid lower-ranked record. */
    uint32_t soff0 = 0;
    if (!bt_u16(nt, 4, &soff0) || (uint64_t)soff0 + off + len > nt.n) {
        return;
    }
    uint32_t score = 0, kind = 0;
    if (pid == 3 && (eid == 1 || eid == 10)) {
        kind = 2;
        score = lang == 0x409 ? 5 : 4;
    } else if (pid == 0) {
        kind = 2;
        score = 3;
    } else if (pid == 3 && eid == 0) {
        kind = 2;
        score = 2;
    } else if (pid == 1 && eid == 0 && lang == 0) {
        /* Mac Roman only for English (language 0); other Mac languages use other scripts. */
        kind = 1;
        score = 1;
    }
    if (score > best->score) {
        best->score = score;
        best->kind = kind;
        best->off = 0;
        best->len = 0;
        /* string storage offset: stringOffset (from storage start) */
        uint32_t soff = 0;
        if (!bt_u16(nt, 4, &soff)) {
            return;
        }
        best->off = soff + off;
        best->len = len;
    }
}

static bool name_find(embi_bytes nt, uint32_t id, name_pick *out)
{
    *out = (name_pick){0};
    uint32_t count = 0;
    if (!bt_u16(nt, 2, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        name_consider(nt, 6 + 12 * i, id, out);
    }
    return out->kind != 0;
}

/* Mac OS Roman bytes 0x80..0xFF as Unicode code points (the Apple table). */
static const uint16_t mac_roman_hi[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4,
    0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF,
    0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020,
    0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4,
    0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202,
    0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1,
    0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3,
    0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A,
    0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC,
    0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF,
    0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7};

/* Copies the picked string as UTF-8 into the font-owned buffer at *pos. */
static bool name_store(embi_bytes nt, const name_pick *p, uint8_t *buf, size_t *pos,
                       embi_str *dst, size_t cap_total)
{
    embi_bytes src;
    if (!bt_sub(nt, p->off, p->len, &src)) {
        return false;
    }
    if (p->kind == 2) {
        size_t units = src.n / 2;
        uint16_t *tmp = units ? embi_alloc(units * sizeof(uint16_t)) : nullptr;
        if (units != 0 && tmp == nullptr) {
            return false;
        }
        for (size_t i = 0; i < units; i++) {
            tmp[i] = (uint16_t)((src.p[2 * i] << 8) | src.p[2 * i + 1]);
        }
        size_t need = embi_utf16_to_utf8(tmp, units, nullptr, 0);
        if (*pos + need > cap_total) {
            if (units) {
                embi_free(tmp, units * sizeof(uint16_t));
            }
            return false;
        }
        embi_utf16_to_utf8(tmp, units, (char *)buf + *pos, need);
        dst->ptr = (const char *)buf + *pos;
        dst->len = (uint32_t)need;
        *pos += need;
        if (units) {
            embi_free(tmp, units * sizeof(uint16_t));
        }
        return true;
    }
    /* Mac Roman: ASCII maps to itself, the upper half through the table below. Every byte is at
     * most 3 UTF-8 bytes, which the caller's per-name budget already covers. */
    if (src.n > (cap_total - *pos) / 3) {
        return false;
    }
    char *w = (char *)buf + *pos;
    size_t k = 0;
    for (size_t i = 0; i < src.n; i++) {
        uint32_t cp = src.p[i] < 0x80 ? src.p[i] : mac_roman_hi[src.p[i] - 0x80u];
        char tmp[4];
        size_t n = embi_utf8_encode(cp, tmp);
        for (size_t j = 0; j < n; j++) {
            w[k++] = tmp[j];
        }
    }
    dst->ptr = w;
    dst->len = (uint32_t)k;
    *pos += k;
    return true;
}

/* Reads family, subfamily and full name into f->metrics and f->name_buf. Falls back from the
 * legacy IDs (1, 2) to the typographic ones (16, 17) and the other way round. */
static bool font_load_names(embi_font *f, embi_bytes nt)
{
    enum { NN = 5 };
    name_pick pk[NN] = {{0}};
    /* family, subfamily, full name, typographic family (16), typographic subfamily (17) */
    uint32_t ids[NN] = {1, 2, 4, 16, 17};
    uint32_t fallback[NN] = {16, 17, 0, 1, 2};
    size_t total = 0;
    for (int i = 0; i < NN; i++) {
        if (!name_find(nt, ids[i], &pk[i]) && fallback[i] != 0) {
            name_find(nt, fallback[i], &pk[i]);
        }
        if (pk[i].kind != 0) {
            /* Worst case: 3 UTF-8 bytes per UTF-16 unit, or 3 per Mac Roman byte. */
            total += (size_t)pk[i].len * 3u + 3u;
        }
    }
    if (total == 0) {
        return true;
    }
    f->name_buf = embi_alloc(total);
    f->name_len = total;
    if (f->name_buf == nullptr) {
        return false;
    }
    size_t pos = 0;
    embi_str *dst[NN] = {&f->metrics.family, &f->metrics.subfamily, &f->metrics.full_name,
                         &f->metrics.typo_family, &f->metrics.typo_subfamily};
    for (int i = 0; i < NN; i++) {
        /* Names are optional: a string that points outside the table leaves that name empty. */
        if (pk[i].kind != 0 && !name_store(nt, &pk[i], f->name_buf, &pos, dst[i], total)) {
            *dst[i] = (embi_str){nullptr, 0};
        }
    }
    return true;
}

/* ------------------------------------------------------------------ cmap */

/* Chooses cmap subtables: format 12 (Unicode full), format 4 (BMP), format 14 (variations). */
static void font_pick_cmap(embi_font *f, embi_bytes cm)
{
    uint32_t n = 0;
    if (!bt_u16(cm, 2, &n)) {
        return;
    }
    uint32_t best4_score = 0, best12_score = 0;
    embi_bytes symbol = {nullptr, 0}; /* (3,0) format 4, used only without a Unicode cmap */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pid = 0, eid = 0, off = 0, fmt = 0;
        uint64_t rec = 4u + 8u * (uint64_t)i;
        if (!bt_u16(cm, rec, &pid) || !bt_u16(cm, rec + 2, &eid) || !bt_u32(cm, rec + 4, &off)) {
            return;
        }
        embi_bytes sub;
        if (!bt_tail(cm, off, &sub) || !bt_u16(sub, 0, &fmt)) {
            continue;
        }
        bool unicode = pid == 0 || (pid == 3 && (eid == 1 || eid == 10));
        if (fmt == 12 && unicode) {
            uint32_t score = (pid == 3 && eid == 10) ? 2 : 1;
            if (score > best12_score) {
                best12_score = score;
                f->cmap12 = sub;
            }
        } else if (fmt == 4 && unicode) {
            uint32_t score = (pid == 3 && eid == 1) ? 2 : 1;
            if (score > best4_score) {
                best4_score = score;
                f->cmap4 = sub;
            }
        } else if (fmt == 4 && pid == 3 && eid == 0 && symbol.n == 0) {
            symbol = sub;
        } else if (fmt == 14 && pid == 0 && eid == 5 && f->cmap14.n == 0) {
            f->cmap14 = sub;
        }
    }
    if (f->cmap4.n == 0 && f->cmap12.n == 0 && symbol.n != 0) {
        f->cmap4 = symbol;
        f->cmap_symbol = true;
    }
}

static uint32_t cmap12_lookup(embi_bytes s, uint32_t cp)
{
    uint32_t ng = 0;
    if (!bt_u32(s, 12, &ng)) {
        return 0;
    }
    uint32_t lo = 0, hi = ng;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t sc = 0, ec = 0, sg = 0;
        uint64_t rec = 16u + 12ull * mid;
        if (!bt_u32(s, rec, &sc) || !bt_u32(s, rec + 4, &ec) || !bt_u32(s, rec + 8, &sg)) {
            return 0;
        }
        if (cp < sc) {
            hi = mid;
        } else if (cp > ec) {
            lo = mid + 1;
        } else {
            uint64_t g = (uint64_t)sg + (cp - sc);
            return g > 0xFFFFFFFFu ? 0 : (uint32_t)g;
        }
    }
    return 0;
}

static uint32_t cmap4_lookup(embi_bytes s, uint32_t cp)
{
    if (cp > 0xFFFF) {
        return 0;
    }
    uint32_t segx2 = 0;
    if (!bt_u16(s, 6, &segx2)) {
        return 0;
    }
    uint32_t seg = segx2 / 2;
    uint64_t ends = 14, starts = ends + segx2 + 2, deltas = starts + segx2,
             ranges = deltas + segx2;
    uint32_t lo = 0, hi = seg;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t ec = 0;
        if (!bt_u16(s, ends + 2ull * mid, &ec)) {
            return 0;
        }
        if (ec < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= seg) {
        return 0;
    }
    uint32_t sc = 0, dl = 0, ro = 0;
    if (!bt_u16(s, starts + 2ull * lo, &sc) || !bt_u16(s, deltas + 2ull * lo, &dl) ||
        !bt_u16(s, ranges + 2ull * lo, &ro)) {
        return 0;
    }
    if (cp < sc) {
        return 0;
    }
    if (ro == 0) {
        return (cp + dl) & 0xFFFFu;
    }
    uint64_t addr = ranges + 2ull * lo + ro + 2ull * (cp - sc);
    uint32_t g = 0;
    if (!bt_u16(s, addr, &g) || g == 0) {
        return 0;
    }
    return (g + dl) & 0xFFFFu;
}

uint32_t embi_font_glyph_index(const embi_font *f, uint32_t cp)
{
    uint32_t g = 0;
    if (f->cmap12.n != 0) {
        g = cmap12_lookup(f->cmap12, cp);
    } else if (f->cmap4.n != 0) {
        if (f->cmap_symbol && cp <= 0xFF) {
            /* Symbol fonts map their characters to U+F000 + code; some also keep raw codes. */
            g = cmap4_lookup(f->cmap4, 0xF000u + cp);
        }
        if (g == 0) {
            g = cmap4_lookup(f->cmap4, cp);
        }
    }
    return g < f->metrics.num_glyphs ? g : 0;
}

bool embi_font_has_codepoint(const embi_font *f, uint32_t cp)
{
    return embi_font_glyph_index(f, cp) != 0;
}

uint32_t embi_font_glyph_variant(const embi_font *f, uint32_t cp, uint32_t selector)
{
    embi_bytes s = f->cmap14;
    uint32_t nrec = 0;
    if (s.n == 0 || !bt_u32(s, 6, &nrec)) {
        return 0;
    }
    uint32_t lo = 0, hi = nrec;
    uint32_t nondef_off = 0;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t vs = 0;
        uint64_t rec = 10u + 11ull * mid;
        if (!bt_u24(s, rec, &vs)) {
            return 0;
        }
        if (vs < selector) {
            lo = mid + 1;
        } else if (vs > selector) {
            hi = mid;
        } else {
            if (!bt_u32(s, rec + 7, &nondef_off)) {
                return 0;
            }
            break;
        }
    }
    if (nondef_off == 0) {
        return 0;
    }
    embi_bytes map;
    uint32_t nmap = 0;
    if (!bt_tail(s, nondef_off, &map) || !bt_u32(map, 0, &nmap)) {
        return 0;
    }
    uint32_t a = 0, b = nmap;
    while (a < b) {
        uint32_t mid = a + (b - a) / 2;
        uint32_t uv = 0;
        uint64_t rec = 4ull + 5ull * mid;
        if (!bt_u24(map, rec, &uv)) {
            return 0;
        }
        if (uv < cp) {
            a = mid + 1;
        } else if (uv > cp) {
            b = mid;
        } else {
            uint32_t g = 0;
            if (!bt_u16(map, rec + 3, &g)) {
                return 0;
            }
            return g < f->metrics.num_glyphs ? g : 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ fvar */

/* Drops the instances (axes stay usable). */
static void fvar_drop_instances(embi_font *f)
{
    if (f->instances != nullptr) {
        embi_free(f->instances, (size_t)f->instance_count * f->axis_count * sizeof(float));
    }
    f->instances = nullptr;
    f->instance_count = 0;
}

/* Drops axes and instances: the font opens as a static font. */
static void fvar_drop_all(embi_font *f)
{
    fvar_drop_instances(f);
    if (f->axes != nullptr) {
        embi_free(f->axes, (size_t)f->axis_count * sizeof(embi_font_axis));
    }
    f->axes = nullptr;
    f->axis_count = 0;
}

/* Lenient like FreeType: a broken fvar degrades variation support, it never fails the open. A
 * default outside [min, max] is clamped into it, and swapped bounds are put in order. */
static void font_load_fvar(embi_font *f, embi_bytes fv)
{
    uint32_t axes_off = 0, nax = 0, axis_size = 0, ninst = 0, inst_size = 0;
    if (!bt_u16(fv, 4, &axes_off) || !bt_u16(fv, 8, &nax) || !bt_u16(fv, 10, &axis_size) ||
        !bt_u16(fv, 12, &ninst) || !bt_u16(fv, 14, &inst_size)) {
        return;
    }
    if (nax == 0 || nax > EMBI_FONT_MAX_AXES || ninst > EMBI_FONT_MAX_INSTANCES ||
        axis_size < 20) {
        return;
    }
    f->axis_count = nax;
    f->axes = embi_calloc(nax, sizeof(embi_font_axis));
    if (f->axes == nullptr) {
        f->axis_count = 0;
        return;
    }
    for (uint32_t i = 0; i < nax; i++) {
        int32_t mn = 0, df = 0, mx = 0;
        uint32_t tag = 0, nid = 0;
        uint64_t r = axes_off + (uint64_t)i * axis_size;
        if (!bt_u32(fv, r, &tag) || !bt_i32(fv, r + 4, &mn) || !bt_i32(fv, r + 8, &df) ||
            !bt_i32(fv, r + 12, &mx) || !bt_u16(fv, r + 18, &nid)) {
            fvar_drop_all(f);
            return;
        }
        if (mn > mx) {
            int32_t t = mn;
            mn = mx;
            mx = t;
        }
        df = df < mn ? mn : (df > mx ? mx : df);
        f->axes[i].tag = tag;
        f->axes[i].min = mn / 65536.0f;
        f->axes[i].def = df / 65536.0f;
        f->axes[i].max = mx / 65536.0f;
        f->axes[i].name_id = (uint16_t)nid;
    }
    if (ninst > 0) {
        uint64_t inst_base = axes_off + (uint64_t)nax * axis_size;
        size_t need_size = 4u + 4u * (size_t)nax;
        if (inst_size < need_size) {
            return; /* axes are fine, the instance list is not */
        }
        f->instances = embi_calloc((size_t)ninst * nax, sizeof(float));
        if (f->instances == nullptr) {
            return;
        }
        f->instance_count = ninst;
        for (uint32_t i = 0; i < ninst; i++) {
            uint64_t r = inst_base + (uint64_t)i * inst_size;
            for (uint32_t a = 0; a < nax; a++) {
                int32_t v = 0;
                if (!bt_i32(fv, r + 4 + 4ull * a, &v)) {
                    fvar_drop_instances(f);
                    return;
                }
                f->instances[(size_t)i * nax + a] = v / 65536.0f;
            }
        }
    }
}

/* ------------------------------------------------------------------ open */

void embi_font_close(embi_font *f)
{
    if (f == nullptr) {
        return;
    }
    embi_cff_close(f->cff_data);
    f->cff_data = nullptr;
    if (f->axes != nullptr) {
        embi_free(f->axes, (size_t)f->axis_count * sizeof(embi_font_axis));
    }
    if (f->instances != nullptr) {
        embi_free(f->instances, (size_t)f->instance_count * f->axis_count * sizeof(float));
    }
    if (f->name_buf != nullptr) {
        embi_free(f->name_buf, f->name_len);
    }
    *f = (embi_font){0};
}

static bool font_open_fail(embi_font *f)
{
    embi_font_close(f);
    return false;
}

bool embi_font_open(const uint8_t *data, size_t len, uint32_t index, embi_font *out)
{
    *out = (embi_font){0};
    if (data == nullptr || len < 12) {
        return false;
    }
    embi_font *f = out;
    f->file.p = data;
    f->file.n = len;

    font_dir d = {.file = f->file};
    uint32_t base = 0;
    if (!font_face_base(f->file, index, &base)) {
        return false;
    }
    d.base = base;
    uint32_t ver = 0;
    if (!bt_u32(f->file, base, &ver)) {
        return false;
    }
    if (ver != 0x00010000u && ver != EMBI_TAG('t', 'r', 'u', 'e') &&
        ver != EMBI_TAG('O', 'T', 'T', 'O')) {
        return false;
    }
    if (!font_dir_check(&d)) {
        return false;
    }

    /* head */
    embi_bytes head;
    uint32_t upm = 0;
    int32_t locfmt = 0;
    int32_t bb[4] = {0};
    if (!font_table(&d, EMBI_TAG('h', 'e', 'a', 'd'), &head) || head.n < 54 ||
        !bt_u16(head, 18, &upm) || !bt_i16(head, 36, &bb[0]) || !bt_i16(head, 38, &bb[1]) ||
        !bt_i16(head, 40, &bb[2]) || !bt_i16(head, 42, &bb[3]) || !bt_i16(head, 50, &locfmt)) {
        return font_open_fail(f);
    }
    if (upm < 16 || upm > 16384) {
        return font_open_fail(f);
    }
    f->metrics.units_per_em = upm;
    f->metrics.x_min = (int16_t)bb[0];
    f->metrics.y_min = (int16_t)bb[1];
    f->metrics.x_max = (int16_t)bb[2];
    f->metrics.y_max = (int16_t)bb[3];

    /* maxp */
    embi_bytes maxp;
    uint32_t ng = 0;
    if (!font_table(&d, EMBI_TAG('m', 'a', 'x', 'p'), &maxp) || !bt_u16(maxp, 4, &ng) ||
        ng == 0) {
        return font_open_fail(f);
    }
    f->metrics.num_glyphs = ng;

    /* hhea and hmtx */
    embi_bytes hhea;
    uint32_t nhm = 0;
    if (!font_table(&d, EMBI_TAG('h', 'h', 'e', 'a'), &hhea) || hhea.n < 36) {
        return font_open_fail(f);
    }
    int32_t v = 0;
    if (!bt_i16(hhea, 4, &v)) {
        return font_open_fail(f);
    }
    f->metrics.hhea_ascender = (int16_t)v;
    if (!bt_i16(hhea, 6, &v)) {
        return font_open_fail(f);
    }
    f->metrics.hhea_descender = (int16_t)v;
    if (!bt_i16(hhea, 8, &v)) {
        return font_open_fail(f);
    }
    f->metrics.hhea_line_gap = (int16_t)v;
    if (!bt_u16(hhea, 34, &nhm) || nhm == 0) {
        return font_open_fail(f);
    }
    if (nhm > ng) {
        nhm = ng; /* lenient: extra long metrics have no glyph */
    }
    f->num_hmetrics = nhm;
    if (!font_table(&d, EMBI_TAG('h', 'm', 't', 'x'), &f->hmtx) ||
        f->hmtx.n < 4ull * nhm + 2ull * (ng - nhm)) {
        return font_open_fail(f);
    }

    /* Outlines: glyf/loca or CFF/CFF2. */
    int glyf_st = font_table_ex(&d, EMBI_TAG('g', 'l', 'y', 'f'), &f->glyf);
    int loca_st = font_table_ex(&d, EMBI_TAG('l', 'o', 'c', 'a'), &f->loca);
    if (glyf_st == TBL_BROKEN || loca_st == TBL_BROKEN) {
        return font_open_fail(f);
    }
    bool have_glyf = glyf_st == TBL_OK;
    bool have_loca = loca_st == TBL_OK;
    if (have_glyf != have_loca) {
        return font_open_fail(f);
    }
    if (have_glyf) {
        if (locfmt > 1) {
            return font_open_fail(f);
        }
        f->loca_long = locfmt == 1;
        uint64_t need = (uint64_t)(ng + 1) * (f->loca_long ? 4u : 2u);
        if (f->loca.n < need) {
            return font_open_fail(f);
        }
    }
    embi_bytes cff_tbl, cff2_tbl;
    int cff_st = font_table_ex(&d, EMBI_TAG('C', 'F', 'F', ' '), &cff_tbl);
    int cff2_st = font_table_ex(&d, EMBI_TAG('C', 'F', 'F', '2'), &cff2_tbl);
    if (cff_st == TBL_BROKEN || (cff_st == TBL_ABSENT && cff2_st == TBL_BROKEN)) {
        return font_open_fail(f);
    }
    if (cff_st == TBL_OK) {
        f->cff = cff_tbl;
        f->metrics.is_cff = true;
    } else if (cff2_st == TBL_OK) {
        cff_tbl = cff2_tbl;
        f->cff = cff_tbl;
        f->metrics.is_cff = true;
        f->metrics.is_cff2 = true;
    }
    if (f->metrics.is_cff && !embi_cff_open(f, f->cff)) {
        return font_open_fail(f);
    }

    /* cmap */
    embi_bytes cmap;
    int cmap_st = font_table_ex(&d, EMBI_TAG('c', 'm', 'a', 'p'), &cmap);
    if (cmap_st == TBL_BROKEN || (cmap_st == TBL_OK && cmap.n < 4)) {
        return font_open_fail(f); /* a cmap that is listed must be usable */
    }
    if (cmap_st == TBL_OK) {
        font_pick_cmap(f, cmap);
    }

    /* OS/2 */
    embi_bytes os2;
    if (font_table(&d, EMBI_TAG('O', 'S', '/', '2'), &os2) && os2.n >= 78) {
        uint32_t u = 0;
        int32_t s = 0;
        uint32_t fs = 0;
        if (bt_u16(os2, 4, &u)) {
            f->metrics.weight_class = (uint16_t)u;
        }
        if (bt_u16(os2, 6, &u)) {
            f->metrics.width_class = (uint16_t)u;
        }
        if (bt_u16(os2, 62, &fs)) {
            f->metrics.italic = (fs & 1u) != 0;
            f->metrics.bold = (fs & (1u << 5)) != 0;
            f->metrics.use_typo_metrics = (fs & (1u << 7)) != 0;
        }
        if (bt_i16(os2, 68, &s)) {
            f->metrics.typo_ascender = (int16_t)s;
        }
        if (bt_i16(os2, 70, &s)) {
            f->metrics.typo_descender = (int16_t)s;
        }
        if (bt_i16(os2, 72, &s)) {
            f->metrics.typo_line_gap = (int16_t)s;
        }
        if (bt_u16(os2, 74, &u)) {
            f->metrics.win_ascent = (uint16_t)u;
        }
        if (bt_u16(os2, 76, &u)) {
            f->metrics.win_descent = (uint16_t)u;
        }
        if (bt_i16(os2, 26, &s)) {
            f->metrics.strikeout_size = (int16_t)s;
        }
        if (bt_i16(os2, 28, &s)) {
            f->metrics.strikeout_position = (int16_t)s;
        }
        uint32_t version = 0;
        if (bt_u16(os2, 0, &version) && version >= 2 && os2.n >= 90) {
            if (bt_i16(os2, 86, &s)) {
                f->metrics.x_height = (int16_t)s;
            }
            if (bt_i16(os2, 88, &s)) {
                f->metrics.cap_height = (int16_t)s;
            }
        }
    }

    /* post */
    embi_bytes post;
    if (font_table(&d, EMBI_TAG('p', 'o', 's', 't'), &post) && post.n >= 16) {
        int32_t ia = 0, up = 0, ut = 0;
        uint32_t fixed = 0;
        if (bt_i32(post, 4, &ia)) {
            f->metrics.italic_angle = ia / 65536.0f;
        }
        if (bt_i16(post, 8, &up)) {
            f->metrics.underline_position = (int16_t)up;
        }
        if (bt_i16(post, 10, &ut)) {
            f->metrics.underline_thickness = (int16_t)ut;
        }
        if (bt_u32(post, 12, &fixed)) {
            f->metrics.fixed_pitch = fixed != 0;
        }
    }

    /* name */
    embi_bytes nt;
    if (font_table(&d, EMBI_TAG('n', 'a', 'm', 'e'), &nt) && nt.n >= 6) {
        if (!font_load_names(f, nt)) {
            return font_open_fail(f);
        }
    }

    /* fvar, avar, gvar, HVAR, MVAR, COLR, CPAL: optional, so a broken one drops that feature. */
    embi_bytes fv;
    if (font_table(&d, EMBI_TAG('f', 'v', 'a', 'r'), &fv)) {
        font_load_fvar(f, fv);
        f->metrics.is_variable = f->axis_count > 0;
    }
    (void)font_table(&d, EMBI_TAG('a', 'v', 'a', 'r'), &f->avar);
    if (font_table(&d, EMBI_TAG('g', 'v', 'a', 'r'), &f->gvar) && f->gvar.n < 20) {
        f->gvar = (embi_bytes){nullptr, 0};
    }
    embi_bytes hv;
    if (font_table(&d, EMBI_TAG('H', 'V', 'A', 'R'), &hv)) {
        uint32_t ivs_off = 0, map_off = 0;
        if (!bt_u32(hv, 4, &ivs_off) || !bt_u32(hv, 8, &map_off) || ivs_off == 0 ||
            !bt_tail(hv, ivs_off, &f->hvar_ivs) ||
            (map_off != 0 && !bt_tail(hv, map_off, &f->hvar_adv_map))) {
            f->hvar_ivs = (embi_bytes){nullptr, 0};
            f->hvar_adv_map = (embi_bytes){nullptr, 0};
        }
    }
    if (font_table(&d, EMBI_TAG('M', 'V', 'A', 'R'), &f->mvar)) {
        uint32_t ivs_off = 0;
        if (!bt_u16(f->mvar, 10, &ivs_off) ||
            (ivs_off != 0 && !bt_tail(f->mvar, ivs_off, &f->mvar_ivs))) {
            f->mvar = (embi_bytes){nullptr, 0};
            f->mvar_ivs = (embi_bytes){nullptr, 0};
        }
    }
    if (font_table(&d, EMBI_TAG('C', 'O', 'L', 'R'), &f->colr)) {
        uint32_t cv = 0;
        if (!bt_u16(f->colr, 0, &cv) || cv > 1) {
            f->colr = (embi_bytes){nullptr, 0};
        } else {
            f->metrics.has_colr = true;
        }
    }
    (void)font_table(&d, EMBI_TAG('C', 'P', 'A', 'L'), &f->cpal);
    /* Bitmap colour glyphs (CBLC/CBDT, sbix): parsed lazily by font_bitmap.c. A table that is cut short is dropped. */
    if (font_table(&d, EMBI_TAG('C', 'B', 'L', 'C'), &f->cblc) &&
        font_table(&d, EMBI_TAG('C', 'B', 'D', 'T'), &f->cbdt)) {
        uint32_t cver = 0;
        if (!bt_u32(f->cblc, 0, &cver) || (cver != 0x00020000u && cver != 0x00030000u)) {
            f->cblc = (embi_bytes){nullptr, 0};
            f->cbdt = (embi_bytes){nullptr, 0};
        }
    } else {
        f->cblc = (embi_bytes){nullptr, 0};
        f->cbdt = (embi_bytes){nullptr, 0};
    }
    if (font_table(&d, EMBI_TAG('s', 'b', 'i', 'x'), &f->sbix)) {
        uint32_t sv = 0;
        if (!bt_u16(f->sbix, 0, &sv) || sv < 1) {
            f->sbix = (embi_bytes){nullptr, 0};
        }
    }

    /* Instances need the axis count; nothing else to finish. */
    f->metrics.num_glyphs = ng;
    return true;
}

bool embi_font_get_metrics(const embi_font *f, embi_font_metrics *out)
{
    if (f == nullptr || out == nullptr || f->metrics.units_per_em == 0) {
        return false;
    }
    *out = f->metrics;
    return true;
}

/* ------------------------------------------------------------------ axes and instances */

int32_t embi_font_axis_index(const embi_font *f, uint32_t tag)
{
    for (uint32_t i = 0; i < f->axis_count; i++) {
        if (f->axes[i].tag == tag) {
            return (int32_t)i;
        }
    }
    return -1;
}

uint32_t embi_font_instance_count(const embi_font *f)
{
    return f->instance_count;
}

bool embi_font_instance_coords(const embi_font *f, uint32_t i, float *user_out)
{
    if (i >= f->instance_count || user_out == nullptr) {
        return false;
    }
    for (uint32_t a = 0; a < f->axis_count; a++) {
        user_out[a] = f->instances[(size_t)i * f->axis_count + a];
    }
    return true;
}
