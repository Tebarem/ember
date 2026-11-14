/*
 * System fonts (W4i): discovery of the platform's font directories through SDL3 only, an index of every face (names,
 * weight, width, style, variation ranges, a coverage summary and the colour flags), the index cache in SDL user
 * storage, the lazy loading of faces (fontdb.c), the generic family tables and the script preferences of the
 * per-character fallback (match.c).
 *
 * The index is built on a job thread (embi_sys_start), so nothing waits for it: until it is ready, matching sees the
 * registered, @font-face and bundled faces only. Once it is ready the index is immutable, except for the loaded faces,
 * which only the main thread touches. Each file is keyed by its path, size and modification time in the cache, so a
 * warm start reads the cache and parses nothing that did not change.
 *
 * Coverage. One bit per 256-code-point block (4352 blocks, 544 bytes per face), set when the cmap maps a code point of
 * the block. It is a superset: a set bit does not prove a glyph, so the per-character fallback (match.c) checks a
 * candidate's cmap alone first (embi_sys_probe: one table read) and loads the whole face only when the cmap maps the
 * code point.
 *
 * A file that parses to no face is kept in the cache as a zero-face record (its path, size and mtime), so a warm start
 * does not parse it again; such records never enter the index.
 */

#include "text/text_int.h"

#include "codec/codec.h"

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_platform.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_storage.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#define EMBI_BLOCKS 4352u /* 0x110000 / 256 */
#define EMBI_COVER_BYTES (EMBI_BLOCKS / 8u)
#define SYS_MAX_FILES 20000u
#define SYS_MAX_DEPTH 8
#define SYS_MAX_DIRS 32u
#define SYS_MAX_ALT 16u
#define SYS_NAME_MAX 256u
#define SYS_TABLE_MAX (8u << 20)
#define SYS_CACHE_FILE "index-v2.bin"
#define SYS_CACHE_MAGIC "EMBIFI02"

#define SYS_FLAG_COLOR 1u  /* COLR, CBDT or sbix */
#define SYS_FLAG_BITMAP 2u /* CBDT or sbix */
#define SYS_FLAG_EMPTY 4u  /* a zero-face record: a file that parsed to no face (cache only, never in the index) */
#define SYS_FACE_NONE 0xFFFFFFFFu

/* One face of a file, as indexed. Owned strings are freed by sysent_clear. */
typedef struct sysent {
    char *path;      /* the file */
    uint32_t face;   /* face index inside the file (TTC) */
    uint64_t size;   /* file size when indexed */
    int64_t mtime;   /* file modification time when indexed (SDL_Time, ns) */
    char *legacy;    /* name ID 1 (English preferred), may be nullptr */
    char *typo;      /* name ID 16, may be nullptr */
    char *full;      /* name ID 4, may be nullptr */
    char *ps;        /* name ID 6, may be nullptr */
    char **alt;      /* localized family names (IDs 1 and 16 of other languages): nalt entries */
    uint32_t nalt;
    uint16_t weight, width;
    uint8_t style;   /* EMBI_STYLE_* */
    uint8_t flags;   /* SYS_FLAG_* */
    float wlo, whi, slo, shi;
    uint8_t *cover;  /* EMBI_COVER_BYTES bits */
    embi_face *loaded; /* the loaded face, or nullptr */
    bool bad;          /* the file did not read or open as a face: not tried again (memory only, never cached) */
} sysent;

/* A name of an entry for the lookup. Family rows: tier 0 typographic, 1 legacy, 2 localized (alt[idx]). Local-name rows:
 * tier 0 full name, 1 PostScript name. */
typedef struct sysrow {
    uint64_t hash;
    uint32_t ent;
    uint32_t tier;
    uint32_t idx;
} sysrow;

struct embi_sysindex {
    sysent *ents;
    uint32_t n, cap;
    sysrow *rows; /* family names, sorted by hash */
    uint32_t nrows;
    sysrow *lrows; /* full and PostScript names, sorted by hash */
    uint32_t nlrows;
    char **dirs; /* the directories to scan (owned copies of the options' list) */
    uint32_t ndirs;
    embi_sys_opts opts;
    SDL_Thread *thread;
    SDL_AtomicInt ready;
    SDL_AtomicInt cancel;
    uint64_t epoch;
    bool warm;
    uint32_t files, reparsed;
    double index_ms;
    uint32_t loaded;      /* system faces loaded now */
    uint64_t loaded_bytes;
    uint64_t index_bytes;
    char cache_name[48]; /* the cache file in the user storage (tests use their own) */
};

/* ------------------------------------------------------------------ small helpers */

static char *str_dup(const char *s, size_t n)
{
    char *p = embi_alloc(n + 1);
    if (n > 0) {
        SDL_memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

static char *str_dup_cstr(const char *s)
{
    return str_dup(s, SDL_strlen(s));
}

static void str_free(char *s)
{
    if (s != nullptr) {
        embi_free(s, SDL_strlen(s) + 1);
    }
}

static uint64_t fold_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c - 'A' + 'a');
        }
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

static bool ascii_ieq(const char *a, size_t an, const char *b, size_t bn)
{
    return an == bn && embi_str_ieq((embi_str){a, (uint32_t)an}, (embi_str){b, (uint32_t)bn});
}

static double ms_since(uint64_t t0_ns)
{
    return (double)(SDL_GetTicksNS() - t0_ns) / 1.0e6;
}

/* True when s starts with the language or script prefix p (case-insensitive), at a subtag boundary. */
static bool lang_prefix(embi_str s, const char *p)
{
    size_t n = SDL_strlen(p);
    if (s.len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != p[i]) {
            return false;
        }
    }
    return s.len == n || s.ptr[n] == '-' || s.ptr[n] == '_';
}

/* ------------------------------------------------------------------ generic families and script preferences */

enum { PLAT_WINDOWS, PLAT_APPLE, PLAT_LINUX, PLAT_ANDROID, PLAT_COUNT, PLAT_NONE = -1 };

static const char *const k_gen_names[EMBI_GEN_COUNT] = {
    "serif",     "sans-serif", "monospace", "cursive",    "fantasy", "system-ui", "ui-serif",
    "ui-sans-serif", "ui-monospace", "ui-rounded", "emoji", "math", "fangsong",
};

/* Windows. */
static const char *const w_serif[] = {"Times New Roman", "Georgia", "Cambria", nullptr};
static const char *const w_sans[] = {"Arial", "Segoe UI", "Tahoma", "Verdana", nullptr};
static const char *const w_mono[] = {"Consolas", "Courier New", "Lucida Console", nullptr};
static const char *const w_cursive[] = {"Comic Sans MS", "Segoe Script", nullptr};
static const char *const w_fantasy[] = {"Impact", "Comic Sans MS", nullptr};
static const char *const w_system[] = {"Segoe UI", "Arial", nullptr};
static const char *const w_uiserif[] = {"Cambria", "Times New Roman", nullptr};
static const char *const w_uimono[] = {"Consolas", "Courier New", nullptr};
static const char *const w_emoji[] = {"Segoe UI Emoji", "Segoe UI Symbol", nullptr};
static const char *const w_math[] = {"Cambria Math", nullptr};
static const char *const w_fangsong[] = {"FangSong", "SimSun", nullptr};

/* Apple (macOS and iOS). */
static const char *const a_serif[] = {"Times", "Times New Roman", "Georgia", nullptr};
static const char *const a_sans[] = {"Helvetica", "Arial", nullptr};
static const char *const a_mono[] = {"Courier", "Menlo", "Courier New", nullptr};
static const char *const a_cursive[] = {"Apple Chancery", "Snell Roundhand", nullptr};
static const char *const a_fantasy[] = {"Papyrus", "Herculanum", nullptr};
static const char *const a_system[] = {"Helvetica Neue", "Helvetica", nullptr};
static const char *const a_uiserif[] = {"New York", "Times", nullptr};
static const char *const a_uimono[] = {"SF Mono", "Menlo", nullptr};
static const char *const a_emoji[] = {"Apple Color Emoji", nullptr};
static const char *const a_math[] = {"STIX Two Math", "Cambria Math", nullptr};
static const char *const a_fangsong[] = {"STFangsong", "Songti SC", nullptr};

/* Linux and the BSDs (the families fontconfig usually resolves to on Debian-like systems). */
static const char *const l_serif[] = {"DejaVu Serif", "Liberation Serif", "Noto Serif", nullptr};
static const char *const l_sans[] = {"DejaVu Sans", "Liberation Sans", "Noto Sans", nullptr};
static const char *const l_mono[] = {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", nullptr};
static const char *const l_cursive[] = {"URW Chancery L", "Comic Neue", "Noto Sans", nullptr};
static const char *const l_fantasy[] = {"Impact", "DejaVu Sans", nullptr};
static const char *const l_system[] = {"Noto Sans", "DejaVu Sans", "Liberation Sans", nullptr};
static const char *const l_uiserif[] = {"Noto Serif", "DejaVu Serif", nullptr};
static const char *const l_uimono[] = {"Noto Sans Mono", "DejaVu Sans Mono", nullptr};
static const char *const l_emoji[] = {"Noto Color Emoji", "Noto Emoji", nullptr};
static const char *const l_math[] = {"Noto Sans Math", "STIX Two Math", "Latin Modern Math", nullptr};
static const char *const l_fangsong[] = {"Noto Serif CJK SC", "Noto Sans CJK SC", nullptr};

/* Android. */
static const char *const d_serif[] = {"Noto Serif", "Droid Serif", nullptr};
static const char *const d_sans[] = {"Roboto", "Noto Sans", "Droid Sans", nullptr};
static const char *const d_mono[] = {"Droid Sans Mono", "Noto Sans Mono", nullptr};
static const char *const d_cursive[] = {"Dancing Script", "Noto Sans", nullptr};
static const char *const d_fantasy[] = {"Roboto", "Noto Sans", nullptr};
static const char *const d_system[] = {"Roboto", "Noto Sans", nullptr};
static const char *const d_uiserif[] = {"Noto Serif", "Droid Serif", nullptr};
static const char *const d_uimono[] = {"Droid Sans Mono", "Noto Sans Mono", nullptr};
static const char *const d_emoji[] = {"Noto Color Emoji", nullptr};
static const char *const d_math[] = {"Noto Sans Math", nullptr};
static const char *const d_fangsong[] = {"Noto Serif CJK SC", nullptr};

/* Indexed by the platform, then by the generic (EMBI_GEN_*). */
static const char *const *const k_gen_tables[PLAT_COUNT][EMBI_GEN_COUNT] = {
    {w_serif, w_sans, w_mono, w_cursive, w_fantasy, w_system, w_uiserif, w_sans, w_uimono, w_system, w_emoji, w_math,
     w_fangsong},
    {a_serif, a_sans, a_mono, a_cursive, a_fantasy, a_system, a_uiserif, a_sans, a_uimono, a_system, a_emoji, a_math,
     a_fangsong},
    {l_serif, l_sans, l_mono, l_cursive, l_fantasy, l_system, l_uiserif, l_sans, l_uimono, l_system, l_emoji, l_math,
     l_fangsong},
    {d_serif, d_sans, d_mono, d_cursive, d_fantasy, d_system, d_uiserif, d_sans, d_uimono, d_system, d_emoji, d_math,
     d_fangsong},
};

static int platform_index(void)
{
    const char *p = SDL_GetPlatform();
    if (p == nullptr) {
        return PLAT_NONE;
    }
    if (SDL_strcmp(p, "Windows") == 0) {
        return PLAT_WINDOWS;
    }
    if (SDL_strcmp(p, "Mac OS X") == 0 || SDL_strcmp(p, "macOS") == 0 || SDL_strcmp(p, "iOS") == 0) {
        return PLAT_APPLE;
    }
    if (SDL_strcmp(p, "Android") == 0) {
        return PLAT_ANDROID;
    }
    if (SDL_strcmp(p, "Linux") == 0 || SDL_strcmp(p, "FreeBSD") == 0 || SDL_strcmp(p, "OpenBSD") == 0 ||
        SDL_strcmp(p, "NetBSD") == 0 || SDL_strcmp(p, "DragonFly") == 0) {
        return PLAT_LINUX;
    }
    return PLAT_NONE;
}

int embi_gen_index(embi_str name)
{
    for (int g = 0; g < EMBI_GEN_COUNT; g++) {
        size_t n = SDL_strlen(k_gen_names[g]);
        if (name.len == n && embi_str_ieq(name, (embi_str){k_gen_names[g], (uint32_t)n})) {
            return g;
        }
    }
    return -1;
}

const char *const *embi_sys_generic_default(uint32_t gen)
{
    int p = platform_index();
    if (p == PLAT_NONE || gen >= EMBI_GEN_COUNT) {
        return nullptr;
    }
    return k_gen_tables[p][gen];
}

static const char *const k_ja[] = {"Noto Sans CJK JP", "Hiragino Sans", "Hiragino Kaku Gothic ProN", "Yu Gothic",
                                   "Meiryo",          "MS Gothic",     "Noto Sans JP",             nullptr};
static const char *const k_sc[] = {"Noto Sans CJK SC", "PingFang SC", "Microsoft YaHei", "SimHei", "Noto Sans SC",
                                   nullptr};
static const char *const k_tc[] = {"Noto Sans CJK TC", "PingFang TC", "Microsoft JhengHei", "Noto Sans TC", nullptr};
static const char *const k_kr[] = {"Noto Sans CJK KR", "Apple SD Gothic Neo", "Malgun Gothic", "Noto Sans KR", nullptr};
static const char *const k_arabic[] = {"Noto Sans Arabic", "Geeza Pro", "Segoe UI", "Arial", "Noto Naskh Arabic",
                                       nullptr};
static const char *const k_hebrew[] = {"Noto Sans Hebrew", "Arial Hebrew", "Segoe UI", "David", "Arial", nullptr};
static const char *const k_devanagari[] = {"Noto Sans Devanagari", "Kohinoor Devanagari", "Nirmala UI", "Mangal",
                                           nullptr};
static const char *const k_bengali[] = {"Noto Sans Bengali", "Kohinoor Bangla", "Nirmala UI", "Vrinda", nullptr};
static const char *const k_tamil[] = {"Noto Sans Tamil", "Tamil MN", "Nirmala UI", "Latha", nullptr};
static const char *const k_thai[] = {"Noto Sans Thai", "Thonburi", "Leelawadee UI", "Tahoma", nullptr};
static const char *const k_armenian[] = {"Noto Sans Armenian", "Mshtakan", "Sylfaen", nullptr};
static const char *const k_georgian[] = {"Noto Sans Georgian", "Sylfaen", nullptr};
static const char *const k_ethiopic[] = {"Noto Sans Ethiopic", "Kefa", nullptr};
static const char *const k_math_sym[] = {"STIX Two Math", "Cambria Math", "Noto Sans Math", "Latin Modern Math", nullptr};
static const char *const k_emoji_sym[] = {"Noto Color Emoji", "Apple Color Emoji", "Segoe UI Emoji", "Twemoji Mozilla",
                                          "Segoe UI Symbol",  "Noto Emoji",        nullptr};

/* Han by language: Japanese, Korean, traditional Chinese for zh-Hant and the regions that use it, simplified Chinese
 * for the other zh tags and for an unknown language. */
static const char *const *han_prefs(embi_str lang)
{
    if (lang_prefix(lang, "ja")) {
        return k_ja;
    }
    if (lang_prefix(lang, "ko")) {
        return k_kr;
    }
    if (lang_prefix(lang, "zh")) {
        if (lang_prefix(lang, "zh-hant") || lang_prefix(lang, "zh-tw") || lang_prefix(lang, "zh-hk") ||
            lang_prefix(lang, "zh-mo")) {
            return k_tc;
        }
    }
    return k_sc;
}

const char *const *embi_sys_script_prefs(uint32_t cp, embi_str lang)
{
    if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF)) {
        return k_emoji_sym;
    }
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x20000 && cp <= 0x2FA1F) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || cp == 0x3005 || cp == 0x3007 || (cp >= 0x3000 && cp <= 0x303F) ||
        (cp >= 0xFF00 && cp <= 0xFFEF)) {
        return han_prefs(lang);
    }
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) {
        return k_ja;
    }
    if ((cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
        (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xD7B0 && cp <= 0xD7FF)) {
        return k_kr;
    }
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF) ||
        (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) {
        return k_arabic;
    }
    if ((cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0xFB1D && cp <= 0xFB4F)) {
        return k_hebrew;
    }
    if ((cp >= 0x0900 && cp <= 0x097F) || (cp >= 0xA8E0 && cp <= 0xA8FF)) {
        return k_devanagari;
    }
    if (cp >= 0x0980 && cp <= 0x09FF) {
        return k_bengali;
    }
    if (cp >= 0x0B80 && cp <= 0x0BFF) {
        return k_tamil;
    }
    if (cp >= 0x0E00 && cp <= 0x0E7F) {
        return k_thai;
    }
    if (cp >= 0x0530 && cp <= 0x058F) {
        return k_armenian;
    }
    if (cp >= 0x10A0 && cp <= 0x10FF) {
        return k_georgian;
    }
    if (cp >= 0x1200 && cp <= 0x137F) {
        return k_ethiopic;
    }
    if (cp >= 0x2200 && cp <= 0x22FF) {
        return k_math_sym;
    }
    return nullptr;
}

/* ------------------------------------------------------------------ byte sources */

/* A font container: a file read through SDL_IOStream, or decoded WOFF bytes in memory. */
typedef struct fsrc {
    SDL_IOStream *io;
    const uint8_t *mem;
    uint64_t n;
} fsrc;

static bool fsrc_read(const fsrc *s, uint64_t off, void *dst, uint64_t n)
{
    if (off > s->n || n > s->n - off) {
        return false;
    }
    if (s->mem != nullptr) {
        SDL_memcpy(dst, s->mem + off, (size_t)n);
        return true;
    }
    if (SDL_SeekIO(s->io, (Sint64)off, SDL_IO_SEEK_SET) < 0) {
        return false;
    }
    return SDL_ReadIO(s->io, dst, (size_t)n) == (size_t)n;
}

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Reads a whole table (SDL memory: the caller frees it). nullptr when it cannot be read. */
static uint8_t *fsrc_table(const fsrc *s, uint64_t off, uint32_t len)
{
    if (len == 0 || len > SYS_TABLE_MAX) {
        return nullptr;
    }
    uint8_t *b = SDL_malloc(len);
    if (b == nullptr || !fsrc_read(s, off, b, len)) {
        SDL_free(b);
        return nullptr;
    }
    return b;
}

typedef struct stab {
    uint64_t off;
    uint32_t len;
    bool has;
} stab;

typedef struct sdir {
    stab name, os2, post, fvar, cmap, colr, cbdt, sbix;
} sdir;

/* The table directory of the sfnt at base. False when it is not an sfnt directory. */
static bool read_dir(const fsrc *s, uint64_t base, sdir *d)
{
    SDL_memset(d, 0, sizeof *d);
    uint8_t h[12];
    if (!fsrc_read(s, base, h, 12)) {
        return false;
    }
    uint32_t tag = be32(h);
    if (tag != 0x00010000u && tag != 0x4F54544Fu /* OTTO */ && tag != 0x74727565u /* true */) {
        return false;
    }
    uint32_t ntab = be16(h + 4);
    if (ntab > 512u) {
        ntab = 512u;
    }
    for (uint32_t i = 0; i < ntab; i++) {
        uint8_t r[16];
        if (!fsrc_read(s, base + 12 + 16ull * i, r, 16)) {
            break;
        }
        stab t = {be32(r + 8), be32(r + 12), true};
        switch (be32(r)) {
        case 0x6E616D65u: d->name = t; break; /* name */
        case 0x4F532F32u: d->os2 = t; break;  /* OS/2 */
        case 0x706F7374u: d->post = t; break; /* post */
        case 0x66766172u: d->fvar = t; break; /* fvar */
        case 0x636D6170u: d->cmap = t; break; /* cmap */
        case 0x434F4C52u: d->colr = t; break; /* COLR */
        case 0x43424454u: d->cbdt = t; break; /* CBDT */
        case 0x73626978u: d->sbix = t; break; /* sbix */
        default: break;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ names, metrics, coverage */

/* Decodes one name record to UTF-8 (owned, nullptr when unusable). UTF-16BE for platforms 0 and 3; the ASCII part of
 * Mac Roman for platform 1. */
static char *decode_name(const uint8_t *str, uint32_t len, uint32_t platform)
{
    if (platform == 1) {
        if (len == 0 || len > SYS_NAME_MAX) {
            return nullptr;
        }
        for (uint32_t i = 0; i < len; i++) {
            if (str[i] >= 0x80 || str[i] == 0) {
                return nullptr;
            }
        }
        return str_dup((const char *)str, len);
    }
    uint32_t count = len / 2;
    if (count == 0) {
        return nullptr;
    }
    if (count > SYS_NAME_MAX) {
        count = SYS_NAME_MAX;
    }
    uint16_t *u = SDL_malloc(sizeof(uint16_t) * count);
    char *out = SDL_malloc((size_t)count * 3u + 1u);
    if (u == nullptr || out == nullptr) {
        SDL_free(u);
        SDL_free(out);
        return nullptr;
    }
    for (uint32_t i = 0; i < count; i++) {
        u[i] = (uint16_t)((str[2 * i] << 8) | str[2 * i + 1]);
    }
    size_t n = embi_utf16_to_utf8(u, count, out, (size_t)count * 3u);
    SDL_free(u);
    char *s = nullptr;
    if (n > 0 && n <= (size_t)count * 3u && SDL_strnlen(out, n) == n) {
        s = str_dup(out, n); /* a name with an embedded NUL is hostile: str_free measures with strlen */
    }
    SDL_free(out);
    return s;
}

/* Whether a name record's language is English (Windows 0x0409, Mac 0). */
static bool name_is_english(uint32_t platform, uint32_t lang)
{
    if (platform == 3) {
        return lang == 0x0409u || lang == 0x0809u;
    }
    if (platform == 1) {
        return lang == 0;
    }
    return platform == 0;
}

/* The names of the four IDs the index keeps: slot 0 is ID 1, 1 is ID 4, 2 is ID 6 and 3 is ID 16. */
typedef struct names {
    char *en[4];  /* the English name of the slot */
    char *any[4]; /* another language's name, used when there is no English one */
    char *alt[SYS_MAX_ALT];
    uint32_t nalt;
} names;

static int name_slot(uint32_t id)
{
    switch (id) {
    case 1: return 0;
    case 4: return 1;
    case 6: return 2;
    case 16: return 3;
    default: return -1;
    }
}

static void names_free(names *nm)
{
    for (int i = 0; i < 4; i++) {
        str_free(nm->en[i]);
        str_free(nm->any[i]);
        nm->en[i] = nm->any[i] = nullptr;
    }
    for (uint32_t i = 0; i < nm->nalt; i++) {
        str_free(nm->alt[i]);
    }
    nm->nalt = 0;
}

static bool alt_has(const names *nm, const char *s, size_t n)
{
    for (uint32_t i = 0; i < nm->nalt; i++) {
        if (ascii_ieq(nm->alt[i], SDL_strlen(nm->alt[i]), s, n)) {
            return true;
        }
    }
    return false;
}

/* Reads name IDs 1, 4, 6 and 16. Another language's family name (IDs 1 and 16) is kept as an alternative name. */
static void read_names(const fsrc *s, const stab *t, names *nm)
{
    if (!t->has || t->len < 6) {
        return;
    }
    uint8_t *tb = fsrc_table(s, t->off, t->len);
    if (tb == nullptr) {
        return;
    }
    uint32_t count = be16(tb + 2);
    uint32_t soff = be16(tb + 4);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t r = 6 + 12ull * i;
        if (r + 12 > t->len) {
            break;
        }
        uint32_t platform = be16(tb + r), lang = be16(tb + r + 4), id = be16(tb + r + 6);
        uint32_t len = be16(tb + r + 8), off = be16(tb + r + 10);
        int slot = name_slot(id);
        if (slot < 0 || len == 0) {
            continue;
        }
        uint64_t at = (uint64_t)soff + off;
        if (at + len > t->len) {
            continue;
        }
        char *str = decode_name(tb + at, len, platform);
        if (str == nullptr) {
            continue;
        }
        if (name_is_english(platform, lang)) {
            if (nm->en[slot] == nullptr) {
                nm->en[slot] = str;
                str = nullptr;
            }
        } else {
            if (nm->any[slot] == nullptr) {
                nm->any[slot] = str_dup_cstr(str);
            }
            if ((slot == 0 || slot == 3) && nm->nalt < SYS_MAX_ALT && !alt_has(nm, str, SDL_strlen(str))) {
                nm->alt[nm->nalt++] = str;
                str = nullptr;
            }
        }
        str_free(str);
    }
    SDL_free(tb);
}

static const char *pick_name(const names *nm, int slot)
{
    if (nm->en[slot] != nullptr) {
        return nm->en[slot];
    }
    return nm->any[slot];
}

static uint16_t width_class_of(uint32_t wc)
{
    return (uint16_t)(wc >= 1 && wc <= 9 ? wc : 5);
}

static float stretch_from_class(uint32_t wc)
{
    static const float table[10] = {100.0f, 50.0f, 62.5f, 75.0f, 87.5f, 100.0f, 112.5f, 125.0f, 150.0f, 200.0f};
    return wc >= 1 && wc <= 9 ? table[wc] : 100.0f;
}

static float clamp_weight(float w)
{
    if (w - w != 0.0f) {
        return 400.0f;
    }
    if (w < 1.0f) {
        return 1.0f;
    }
    if (w > 1000.0f) {
        return 1000.0f;
    }
    return (float)(int)(w + 0.5f);
}

/* Sets the blocks of the code points lo..hi (inclusive). */
static void cover_range(uint8_t *bits, uint32_t lo, uint32_t hi)
{
    if (hi > 0x10FFFFu) {
        hi = 0x10FFFFu;
    }
    for (uint32_t b = lo >> 8; lo <= hi && b <= (hi >> 8); b++) {
        bits[b >> 3] |= (uint8_t)(1u << (b & 7u));
    }
}

/* The offset of the best Unicode subtable of a cmap table (tb, len bytes), or -1. Preference: (3,10) full Unicode,
 * (3,1) BMP, (0,*) Unicode, (3,0) symbol, (1,0) MacRoman (only its ASCII half is taken as Unicode). *mac tells the
 * last case. */
static int64_t cmap_best(const uint8_t *tb, uint32_t len, bool *mac)
{
    uint32_t n = be16(tb + 2);
    int64_t best_off = -1;
    int best_rank = 99;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t r = 4 + 8ull * i;
        if (r + 8 > len) {
            break;
        }
        uint32_t pid = be16(tb + r), eid = be16(tb + r + 2), off = be32(tb + r + 4);
        int rank = 99;
        if (pid == 3 && eid == 10) {
            rank = 0;
        } else if (pid == 3 && eid == 1) {
            rank = 1;
        } else if (pid == 0) {
            rank = 2;
        } else if (pid == 3 && eid == 0) {
            rank = 3;
        } else if (pid == 1 && eid == 0) {
            rank = 4;
        }
        if (rank < best_rank && off + 4ull <= len) {
            best_rank = rank;
            best_off = off;
        }
    }
    *mac = best_rank == 4;
    return best_off;
}

/* The coverage summary of the cmap: the blocks of its best subtable (a superset, see the file comment). */
static void read_cmap(const fsrc *s, const stab *t, uint8_t *bits)
{
    if (!t->has || t->len < 4) {
        return;
    }
    uint8_t *tb = fsrc_table(s, t->off, t->len);
    if (tb == nullptr) {
        return;
    }
    bool mac = false;
    int64_t best_off = cmap_best(tb, t->len, &mac);
    if (best_off >= 0) {
        uint64_t sub = (uint64_t)best_off;
        uint32_t fmt = be16(tb + sub);
        if (fmt == 4 && t->len - sub >= 16) {
            uint32_t seg = be16(tb + sub + 6) / 2u;
            uint64_t ends = sub + 14, starts = sub + 16 + 2ull * seg;
            if (ends + 2ull * seg <= t->len && starts + 2ull * seg <= t->len) {
                for (uint32_t k = 0; k < seg; k++) {
                    uint32_t e = be16(tb + ends + 2ull * k);
                    uint32_t st = be16(tb + starts + 2ull * k);
                    if (st <= e && e != 0xFFFFu) {
                        cover_range(bits, st, e);
                    }
                }
            }
        } else if (fmt == 6 && t->len - sub >= 10) {
            uint32_t first = be16(tb + sub + 6), cnt = be16(tb + sub + 8);
            if (cnt > 0 && sub + 10 + 2ull * cnt <= t->len) {
                cover_range(bits, first, first + cnt - 1);
            }
        } else if (fmt == 0 && t->len - sub >= 262) {
            for (uint32_t c = 1; c < 128; c++) {
                if (tb[sub + 6 + c] != 0) {
                    cover_range(bits, c, c);
                }
            }
        } else if ((fmt == 12 || fmt == 13) && t->len - sub >= 16) {
            uint64_t ng = be32(tb + sub + 12);
            uint64_t avail = (t->len - sub - 16) / 12u;
            if (ng > avail) {
                ng = avail;
            }
            for (uint64_t k = 0; k < ng; k++) {
                uint64_t g = sub + 16 + 12ull * k;
                cover_range(bits, be32(tb + g), be32(tb + g + 4));
            }
        }
    }
    SDL_free(tb);
}

/* Whether the best subtable of a cmap table maps cp to a glyph other than .notdef (the exact lookup of a probe). */
static bool cmap_maps(const uint8_t *tb, uint32_t len, uint32_t cp)
{
    if (len < 4) {
        return false;
    }
    bool mac = false;
    int64_t best_off = cmap_best(tb, len, &mac);
    if (best_off < 0) {
        return false;
    }
    uint64_t sub = (uint64_t)best_off;
    uint32_t fmt = be16(tb + sub);
    if (fmt == 4) {
        if (cp > 0xFFFFu || len - sub < 16) {
            return false;
        }
        uint32_t seg = be16(tb + sub + 6) / 2u;
        uint64_t ends = sub + 14, starts = ends + 2ull * seg + 2, deltas = starts + 2ull * seg;
        uint64_t ros = deltas + 2ull * seg;
        if (ros + 2ull * seg > len) {
            return false;
        }
        uint32_t lo = 0, hi = seg;
        while (lo < hi) { /* the first segment whose end is >= cp (ends are sorted) */
            uint32_t mid = lo + (hi - lo) / 2;
            if (be16(tb + ends + 2ull * mid) < cp) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == seg || be16(tb + starts + 2ull * lo) > cp) {
            return false;
        }
        uint32_t delta = be16(tb + deltas + 2ull * lo), ro = be16(tb + ros + 2ull * lo);
        uint32_t g;
        if (ro == 0) {
            g = (cp + delta) & 0xFFFFu;
        } else {
            uint64_t at = ros + 2ull * lo + ro + 2ull * (cp - be16(tb + starts + 2ull * lo));
            if (at + 2 > len) {
                return false;
            }
            g = be16(tb + at);
            if (g != 0) {
                g = (g + delta) & 0xFFFFu;
            }
        }
        return g != 0;
    }
    if (fmt == 6) {
        if (len - sub < 10) {
            return false;
        }
        uint32_t first = be16(tb + sub + 6), cnt = be16(tb + sub + 8);
        if (cp < first || cp - first >= cnt || sub + 10 + 2ull * (cp - first) + 2 > len) {
            return false;
        }
        return be16(tb + sub + 10 + 2ull * (cp - first)) != 0;
    }
    if (fmt == 0) {
        return len - sub >= 262 && cp < (mac ? 128u : 256u) && tb[sub + 6 + cp] != 0;
    }
    if ((fmt == 12 || fmt == 13) && len - sub >= 16) {
        uint64_t ng = be32(tb + sub + 12);
        uint64_t avail = (len - sub - 16) / 12u;
        if (ng > avail) {
            ng = avail;
        }
        uint64_t lo = 0, hi = ng;
        while (lo < hi) { /* the first group whose end is >= cp (groups are sorted) */
            uint64_t mid = lo + (hi - lo) / 2;
            if (be32(tb + sub + 16 + 12 * mid + 4) < cp) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == ng) {
            return false;
        }
        const uint8_t *g = tb + sub + 16 + 12 * lo;
        if (be32(g) > cp) {
            return false;
        }
        uint32_t gid = fmt == 12 ? be32(g + 8) + (cp - be32(g)) : be32(g + 8);
        return gid != 0;
    }
    return false;
}

/* ------------------------------------------------------------------ parsing one face */

/* Fills out with the index entry of the face at base. False when the face has no usable table directory. */
static bool parse_face(const fsrc *s, uint64_t base, uint32_t face_index, sysent *out)
{
    sdir d;
    if (!read_dir(s, base, &d)) {
        return false;
    }
    SDL_memset(out, 0, sizeof *out);
    out->face = face_index;

    /* OS/2: weight class, width class and fsSelection (bit 0 italic, bit 9 oblique). */
    uint32_t wc = 400, wdt = 5, fs = 0;
    if (d.os2.has && d.os2.len >= 64) {
        uint8_t o[64];
        if (fsrc_read(s, d.os2.off, o, 64)) {
            wc = be16(o + 4);
            wdt = be16(o + 6);
            fs = be16(o + 62);
        }
    }
    if (wc == 0) {
        wc = 400;
    }
    out->weight = (uint16_t)wc;
    out->width = width_class_of(wdt);

    /* post: the italic angle (a face with one is italic, as fontdb.c decides). */
    float italic_angle = 0.0f;
    if (d.post.has && d.post.len >= 8) {
        uint8_t pb[8];
        if (fsrc_read(s, d.post.off, pb, 8)) {
            italic_angle = (float)(int32_t)be32(pb + 4) / 65536.0f;
        }
    }
    out->style = EMBI_STYLE_NORMAL;
    if ((fs & 1u) != 0 || italic_angle != 0.0f) {
        out->style = EMBI_STYLE_ITALIC;
    }
    out->wlo = out->whi = clamp_weight((float)wc);
    out->slo = out->shi = stretch_from_class(wdt);

    /* fvar: the wght and wdth ranges. A slnt axis makes an upright face oblique-capable, as in fontdb.c. */
    bool has_slnt = false;
    if (d.fvar.has && d.fvar.len >= 16) {
        uint8_t fh[16];
        if (fsrc_read(s, d.fvar.off, fh, 16)) {
            uint32_t aoff = be16(fh + 4), nax = be16(fh + 8), asz = be16(fh + 10);
            if (nax > 64) {
                nax = 64;
            }
            for (uint32_t a = 0; a < nax; a++) {
                uint8_t ar[20];
                uint64_t at = (uint64_t)aoff + (uint64_t)a * (asz >= 20 ? asz : 20u);
                if (at + 20 > d.fvar.len || !fsrc_read(s, d.fvar.off + at, ar, 20)) {
                    break;
                }
                uint32_t tag = be32(ar);
                float mn = (float)(int32_t)be32(ar + 4) / 65536.0f;
                float mx = (float)(int32_t)be32(ar + 12) / 65536.0f;
                if (tag == 0x77676874u) { /* wght */
                    out->wlo = clamp_weight(mn);
                    out->whi = clamp_weight(mx);
                } else if (tag == 0x77647468u) { /* wdth */
                    out->slo = mn;
                    out->shi = mx;
                } else if (tag == 0x736C6E74u) { /* slnt */
                    has_slnt = true;
                }
            }
        }
    }
    if (out->style == EMBI_STYLE_NORMAL && has_slnt) {
        out->style = EMBI_STYLE_OBLIQUE;
    }

    /* Names: the chosen names, and the alternatives for the family lookup. */
    names nm;
    SDL_memset(&nm, 0, sizeof nm);
    read_names(s, &d.name, &nm);
    const char *v;
    if ((v = pick_name(&nm, 0)) != nullptr) {
        out->legacy = str_dup_cstr(v);
    }
    if ((v = pick_name(&nm, 3)) != nullptr) {
        out->typo = str_dup_cstr(v);
    }
    if ((v = pick_name(&nm, 1)) != nullptr) {
        out->full = str_dup_cstr(v);
    }
    if ((v = pick_name(&nm, 2)) != nullptr) {
        out->ps = str_dup_cstr(v);
    }
    if (nm.nalt > 0) {
        out->alt = embi_alloc(sizeof(char *) * nm.nalt);
        for (uint32_t i = 0; i < nm.nalt; i++) {
            out->alt[i] = nm.alt[i];
            nm.alt[i] = nullptr;
        }
        out->nalt = nm.nalt;
        nm.nalt = 0;
    }
    names_free(&nm);

    out->cover = embi_calloc(1, EMBI_COVER_BYTES);
    read_cmap(s, &d.cmap, out->cover);
    if (d.colr.has) {
        out->flags |= SYS_FLAG_COLOR;
    }
    if (d.cbdt.has || d.sbix.has) {
        out->flags |= SYS_FLAG_COLOR | SYS_FLAG_BITMAP;
    }
    return true;
}

static void sysent_clear(sysent *e)
{
    str_free(e->path);
    str_free(e->legacy);
    str_free(e->typo);
    str_free(e->full);
    str_free(e->ps);
    for (uint32_t i = 0; i < e->nalt; i++) {
        str_free(e->alt[i]);
    }
    if (e->alt != nullptr) {
        embi_free(e->alt, sizeof(char *) * e->nalt);
    }
    if (e->cover != nullptr) {
        embi_free(e->cover, EMBI_COVER_BYTES);
    }
    SDL_memset(e, 0, sizeof *e);
}

/* Appends the index entries of every face of a file to the list. Returns the number of faces added. */
static uint32_t parse_file(const char *path, uint64_t size, int64_t mtime, sysent **list, uint32_t *n, uint32_t *cap)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (io == nullptr) {
        return 0;
    }
    uint8_t magic[4];
    if (SDL_ReadIO(io, magic, 4) != 4) {
        SDL_CloseIO(io);
        return 0;
    }
    fsrc src;
    SDL_memset(&src, 0, sizeof src);
    src.io = io;
    src.n = size;
    uint8_t *decoded = nullptr;
    bool woff = magic[0] == 'w' && magic[1] == 'O' && magic[2] == 'F' && (magic[3] == 'F' || magic[3] == '2');
    if (woff) {
        /* WOFF and WOFF2 are decoded whole (their tables are compressed), then read from memory. */
        uint8_t *raw = size > 0 && size <= SYS_TABLE_MAX * 4u ? SDL_malloc((size_t)size) : nullptr;
        embi_buf out = {0};
        bool ok = raw != nullptr && SDL_SeekIO(io, 0, SDL_IO_SEEK_SET) >= 0 &&
                  SDL_ReadIO(io, raw, (size_t)size) == (size_t)size;
        if (ok) {
            ok = magic[3] == 'F' ? embi_woff1_decode(raw, (size_t)size, SYS_TABLE_MAX * 4u, &out)
                                 : embi_woff2_decode(raw, (size_t)size, SYS_TABLE_MAX * 4u, &out);
        }
        SDL_free(raw);
        if (!ok || out.len == 0) {
            embi_buf_free(&out);
            SDL_CloseIO(io);
            return 0;
        }
        decoded = SDL_malloc(out.len);
        if (decoded != nullptr) {
            SDL_memcpy(decoded, out.data, out.len);
            src.mem = decoded;
            src.n = out.len;
        }
        embi_buf_free(&out);
        if (decoded == nullptr) {
            SDL_CloseIO(io);
            return 0;
        }
    }
    bool ttc = !woff && magic[0] == 't' && magic[1] == 't' && magic[2] == 'c' && magic[3] == 'f';
    uint32_t faces = 1;
    if (ttc) {
        uint8_t th[12];
        faces = fsrc_read(&src, 0, th, 12) ? be32(th + 8) : 0;
        if (faces > 256u) {
            faces = 256u;
        }
    }
    uint32_t added = 0;
    for (uint32_t i = 0; i < faces; i++) {
        uint64_t base = 0;
        if (ttc) {
            uint8_t ob[4];
            if (!fsrc_read(&src, 12 + 4ull * i, ob, 4)) {
                break;
            }
            base = be32(ob);
        }
        if (*n == *cap) {
            uint32_t ncap = *cap == 0 ? 8u : *cap * 2u;
            *list = embi_realloc(*list, sizeof(sysent) * (*cap), sizeof(sysent) * ncap);
            *cap = ncap;
        }
        sysent *e = &(*list)[*n];
        if (!parse_face(&src, base, i, e)) {
            continue;
        }
        e->path = str_dup_cstr(path);
        e->size = size;
        e->mtime = mtime;
        (*n)++;
        added++;
    }
    SDL_free(decoded);
    SDL_CloseIO(io);
    return added;
}

/* ------------------------------------------------------------------ discovery */

typedef struct sfile {
    char *path;
    uint64_t size;
    int64_t mtime;
} sfile;

typedef struct flist {
    sfile *v;
    uint32_t n, cap;
} flist;

typedef struct walk_state {
    flist *list;
    int depth;
    const embi_sysindex *x; /* for the cancel flag; nullptr for a plain listing */
    const char *suffix;     /* nullptr: font files; else only names with this suffix are listed */
} walk_state;

static bool font_file_name(const char *name)
{
    const char *dot = SDL_strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    static const char *const exts[] = {"ttf", "otf", "ttc", "otc", "woff", "woff2"};
    for (uint32_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        if (SDL_strcasecmp(dot + 1, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void flist_push(flist *l, char *path, uint64_t size, int64_t mtime)
{
    if (l->n >= SYS_MAX_FILES) {
        str_free(path);
        return;
    }
    if (l->n == l->cap) {
        uint32_t ncap = l->cap == 0 ? 64u : l->cap * 2u;
        l->v = embi_realloc(l->v, sizeof(sfile) * l->cap, sizeof(sfile) * ncap);
        l->cap = ncap;
    }
    l->v[l->n++] = (sfile){path, size, mtime};
}

static SDL_EnumerationResult walk_cb(void *ud, const char *dir, const char *fname)
{
    walk_state *ws = ud;
    if (ws->x != nullptr && SDL_GetAtomicInt((SDL_AtomicInt *)&ws->x->cancel) != 0) {
        return SDL_ENUM_SUCCESS;
    }
    if (SDL_strcmp(fname, ".") == 0 || SDL_strcmp(fname, "..") == 0) {
        return SDL_ENUM_CONTINUE;
    }
    char *full = nullptr;
    if (SDL_asprintf(&full, "%s/%s", dir, fname) < 0 || full == nullptr) {
        return SDL_ENUM_CONTINUE;
    }
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(full, &info)) {
        SDL_free(full);
        return SDL_ENUM_CONTINUE;
    }
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (ws->suffix == nullptr && ws->depth < SYS_MAX_DEPTH) {
            walk_state child = {ws->list, ws->depth + 1, ws->x, ws->suffix};
            SDL_EnumerateDirectory(full, walk_cb, &child);
        }
        SDL_free(full);
    } else if (info.type == SDL_PATHTYPE_FILE) {
        bool take;
        if (ws->suffix != nullptr) {
            size_t fl = SDL_strlen(fname), sl = SDL_strlen(ws->suffix);
            take = fl >= sl && SDL_strcmp(fname + fl - sl, ws->suffix) == 0;
        } else {
            take = font_file_name(fname);
        }
        if (take) {
            flist_push(ws->list, str_dup_cstr(full), info.size, (int64_t)info.modify_time);
        }
        SDL_free(full);
    } else {
        SDL_free(full);
    }
    return SDL_ENUM_CONTINUE;
}

static int sfile_cmp(const void *a, const void *b)
{
    return SDL_strcmp(((const sfile *)a)->path, ((const sfile *)b)->path);
}

/* A string made by SDL_asprintf, copied into embi memory (the SDL copy is freed here), or nullptr. */
static char *take_asprintf(char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    char *d = str_dup_cstr(s);
    SDL_free(s);
    return d;
}

/* Adds an embi-owned directory string to the list (a duplicate or a full list frees it). */
static void dirs_add(char ***dirs, uint32_t *nd, uint32_t *cap, char *dir)
{
    if (dir == nullptr) {
        return;
    }
    bool dup = false;
    for (uint32_t i = 0; i < *nd && !dup; i++) {
        dup = SDL_strcmp((*dirs)[i], dir) == 0;
    }
    if (dup || *nd >= SYS_MAX_DIRS) {
        str_free(dir);
        return;
    }
    if (*nd == *cap) {
        uint32_t ncap = *cap == 0 ? 8u : *cap * 2u;
        *dirs = embi_realloc(*dirs, sizeof(char *) * (*cap), sizeof(char *) * ncap);
        *cap = ncap;
    }
    (*dirs)[(*nd)++] = dir;
}

/* The <dir> entries of a fontconfig file (~ is the home directory; other relative paths are skipped). */
static void scan_conf(const char *path, char ***dirs, uint32_t *nd, uint32_t *cap)
{
    size_t n = 0;
    char *buf = (char *)SDL_LoadFile(path, &n);
    if (buf == nullptr) {
        return;
    }
    const char *home = SDL_getenv("HOME");
    char *p = buf;
    for (;;) {
        if (*nd >= SYS_MAX_DIRS) {
            break;
        }
        /* XML comments are skipped whole: a commented-out <dir> names no directory. */
        char *cm = SDL_strstr(p, "<!--");
        char *dr = SDL_strstr(p, "<dir");
        if (cm != nullptr && (dr == nullptr || cm < dr)) {
            char *ce = SDL_strstr(cm + 4, "-->");
            if (ce == nullptr) {
                break;
            }
            p = ce + 3;
            continue;
        }
        if (dr == nullptr) {
            break;
        }
        p = dr;
        char c = p[4];
        if (c != '>' && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            p += 4;
            continue;
        }
        char *gt = SDL_strchr(p, '>');
        if (gt == nullptr) {
            break;
        }
        if (gt[-1] == '/') {
            p = gt + 1; /* a self-closing tag names no directory */
            continue;
        }
        char *endtag = SDL_strstr(gt + 1, "</dir>");
        if (endtag == nullptr) {
            break;
        }
        char *a = gt + 1, *b = endtag;
        while (a < b && (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n')) {
            a++;
        }
        while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r' || b[-1] == '\n')) {
            b--;
        }
        size_t len = (size_t)(b - a);
        if (len > 0 && len < 1024 && *a != '<') {
            char *dir = nullptr;
            if (a[0] == '~' && home != nullptr && (len == 1 || a[1] == '/')) {
                char *h = nullptr;
                if (SDL_asprintf(&h, "%s%.*s", home, (int)(len - 1), a + 1) >= 0) {
                    dir = take_asprintf(h);
                }
            } else if (a[0] == '/') {
                dir = str_dup(a, len);
            }
            dirs_add(dirs, nd, cap, dir);
        }
        p = endtag + 6;
    }
    SDL_free(buf);
}

/* Joins a directory and a relative part into an embi string (nullptr when home or the part is missing). */
static char *dir_join(const char *base, const char *rest)
{
    if (base == nullptr) {
        return nullptr;
    }
    char *s = nullptr;
    if (SDL_asprintf(&s, "%s%s", base, rest) < 0) {
        return nullptr;
    }
    return take_asprintf(s);
}

/* The platform's font directories (W4.md section 10). Returns the count. *out receives an embi array of *cap_out
 * capacity; the strings are owned by the caller. */
static uint32_t platform_dirs(char ***out, uint32_t *cap_out)
{
    char **dirs = nullptr;
    uint32_t nd = 0, cap = 0;
    int p = platform_index();
    const char *home = SDL_getenv("HOME");
    if (p == PLAT_WINDOWS) {
        const char *windir = SDL_getenv("WINDIR");
        char *s = nullptr;
        if (SDL_asprintf(&s, "%s/Fonts", windir != nullptr ? windir : "C:/Windows") >= 0) {
            dirs_add(&dirs, &nd, &cap, take_asprintf(s));
        }
        const char *local = SDL_getenv("LOCALAPPDATA");
        if (local != nullptr) {
            char *l = nullptr;
            if (SDL_asprintf(&l, "%s/Microsoft/Windows/Fonts", local) >= 0) {
                dirs_add(&dirs, &nd, &cap, take_asprintf(l));
            }
        }
    } else if (p == PLAT_APPLE) {
        dirs_add(&dirs, &nd, &cap, str_dup_cstr("/System/Library/Fonts"));
        dirs_add(&dirs, &nd, &cap, str_dup_cstr("/System/Library/Fonts/Supplemental"));
        dirs_add(&dirs, &nd, &cap, str_dup_cstr("/Library/Fonts"));
        dirs_add(&dirs, &nd, &cap, dir_join(home, "/Library/Fonts"));
    } else if (p == PLAT_ANDROID) {
        dirs_add(&dirs, &nd, &cap, str_dup_cstr("/system/fonts"));
        dirs_add(&dirs, &nd, &cap, str_dup_cstr("/product/fonts"));
    } else if (p == PLAT_LINUX) {
        scan_conf("/etc/fonts/fonts.conf", &dirs, &nd, &cap);
        /* conf.d: every *.conf file in the directory (the listing is not recursive). */
        flist confs = {nullptr, 0, 0};
        walk_state ws = {&confs, 0, nullptr, ".conf"};
        SDL_EnumerateDirectory("/etc/fonts/conf.d", walk_cb, &ws);
        for (uint32_t i = 0; i < confs.n; i++) {
            scan_conf(confs.v[i].path, &dirs, &nd, &cap);
            str_free(confs.v[i].path);
        }
        embi_free(confs.v, sizeof(sfile) * confs.cap);

        const char *xdg = SDL_getenv("XDG_DATA_HOME");
        if (xdg != nullptr && xdg[0] == '/') {
            dirs_add(&dirs, &nd, &cap, dir_join(xdg, "/fonts"));
        } else {
            dirs_add(&dirs, &nd, &cap, dir_join(home, "/.local/share/fonts"));
        }
        dirs_add(&dirs, &nd, &cap, dir_join(home, "/.fonts"));
        dirs_add(&dirs, &nd, &cap, str_dup_cstr("/usr/share/fonts"));
        dirs_add(&dirs, &nd, &cap, str_dup_cstr("/usr/local/share/fonts"));
    }
    *out = dirs;
    *cap_out = cap;
    return nd;
}

/* ------------------------------------------------------------------ the index cache */

typedef struct wbuf {
    uint8_t *p;
    size_t n, cap;
    bool fail; /* out of memory: nothing more is written, and the buffer is not saved */
} wbuf;

static void w_bytes(wbuf *w, const void *src, size_t n)
{
    if (w->fail) {
        return;
    }
    if (w->n + n > w->cap) {
        size_t cap = w->cap == 0 ? 4096u : w->cap;
        while (cap < w->n + n) {
            cap *= 2u;
        }
        uint8_t *p = SDL_realloc(w->p, cap);
        if (p == nullptr) {
            w->fail = true;
            return;
        }
        w->p = p;
        w->cap = cap;
    }
    SDL_memcpy(w->p + w->n, src, n);
    w->n += n;
}

static void w_u8(wbuf *w, uint8_t v)
{
    w_bytes(w, &v, 1);
}

static void w_u16(wbuf *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    w_bytes(w, b, 2);
}

static void w_u32(wbuf *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    w_bytes(w, b, 4);
}

static void w_u64(wbuf *w, uint64_t v)
{
    w_u32(w, (uint32_t)v);
    w_u32(w, (uint32_t)(v >> 32));
}

static void w_f32(wbuf *w, float f)
{
    uint32_t v;
    SDL_memcpy(&v, &f, 4);
    w_u32(w, v);
}

/* A string: its length, or 0xFFFFFFFF for nullptr, then the bytes. */
static void w_str(wbuf *w, const char *s)
{
    if (s == nullptr) {
        w_u32(w, 0xFFFFFFFFu);
        return;
    }
    size_t n = SDL_strlen(s);
    w_u32(w, (uint32_t)n);
    w_bytes(w, s, n);
}

typedef struct rbuf {
    const uint8_t *p;
    size_t n, at;
    bool fail;
} rbuf;

static bool r_take(rbuf *r, size_t n)
{
    if (r->fail || n > r->n - r->at) {
        r->fail = true;
        return false;
    }
    return true;
}

static uint32_t r_u32(rbuf *r)
{
    if (!r_take(r, 4)) {
        return 0;
    }
    uint32_t v = (uint32_t)r->p[r->at] | ((uint32_t)r->p[r->at + 1] << 8) | ((uint32_t)r->p[r->at + 2] << 16) |
                 ((uint32_t)r->p[r->at + 3] << 24);
    r->at += 4;
    return v;
}

static uint16_t r_u16(rbuf *r)
{
    if (!r_take(r, 2)) {
        return 0;
    }
    uint16_t v = (uint16_t)(r->p[r->at] | (r->p[r->at + 1] << 8));
    r->at += 2;
    return v;
}

static uint8_t r_u8(rbuf *r)
{
    if (!r_take(r, 1)) {
        return 0;
    }
    return r->p[r->at++];
}

static uint64_t r_u64(rbuf *r)
{
    uint64_t lo = r_u32(r);
    return lo | ((uint64_t)r_u32(r) << 32);
}

static float r_f32(rbuf *r)
{
    uint32_t v = r_u32(r);
    float f;
    SDL_memcpy(&f, &v, 4);
    return f;
}

/* A string as written by w_str (owned), nullptr for the null marker. Sets fail on bad data. */
static char *r_str(rbuf *r)
{
    uint32_t n = r_u32(r);
    if (n == 0xFFFFFFFFu) {
        return nullptr;
    }
    if (n > SYS_TABLE_MAX || !r_take(r, n)) {
        r->fail = true;
        return nullptr;
    }
    char *s = str_dup((const char *)r->p + r->at, n);
    r->at += n;
    return s;
}

static const uint8_t *cache_magic(void)
{
    return (const uint8_t *)SYS_CACHE_MAGIC;
}

/* Serializes the entries (sorted by path, then face) into w. */
static void cache_write(wbuf *w, const sysent *ents, uint32_t n)
{
    w_bytes(w, cache_magic(), 8);
    w_u32(w, n);
    for (uint32_t i = 0; i < n; i++) {
        const sysent *e = &ents[i];
        w_str(w, e->path);
        w_u32(w, e->face);
        w_u64(w, e->size);
        w_u64(w, (uint64_t)e->mtime);
        w_str(w, e->legacy);
        w_str(w, e->typo);
        w_str(w, e->full);
        w_str(w, e->ps);
        w_u32(w, e->nalt);
        for (uint32_t k = 0; k < e->nalt; k++) {
            w_str(w, e->alt[k]);
        }
        w_u16(w, e->weight);
        w_u16(w, e->width);
        w_u8(w, e->style);
        w_u8(w, e->flags);
        w_f32(w, e->wlo);
        w_f32(w, e->whi);
        w_f32(w, e->slo);
        w_f32(w, e->shi);
        if ((e->flags & SYS_FLAG_EMPTY) == 0) {
            w_bytes(w, e->cover, EMBI_COVER_BYTES); /* a zero-face record has no coverage */
        }
    }
}

static int sent_cmp(const void *a, const void *b)
{
    const sysent *x = a, *y = b;
    int c = SDL_strcmp(x->path, y->path);
    if (c != 0) {
        return c;
    }
    return x->face < y->face ? -1 : x->face > y->face ? 1 : 0;
}

/* Parses a cache. On any error nothing is kept (the index is then built cold). */
static bool cache_read(const uint8_t *buf, size_t len, sysent **out, uint32_t *nout)
{
    rbuf r = {buf, len, 0, false};
    if (!r_take(&r, 12) || SDL_memcmp(buf, cache_magic(), 8) != 0) {
        return false;
    }
    r.at = 8;
    uint32_t n = r_u32(&r);
    if (n > SYS_MAX_FILES * 64u) {
        return false;
    }
    /* The smallest entry: 5 string headers, face, size, mtime, nalt, weight..flags and 4 floats (a zero-face record has
     * no coverage bitmap). */
    const size_t min_entry = 5u * 4u + 4u + 8u + 8u + 4u + 6u + 16u;
    if ((size_t)n > (len - 12u) / min_entry) {
        return false; /* a file this short cannot hold n entries: nothing is allocated for it */
    }
    sysent *ents = n > 0 ? embi_alloc(sizeof(sysent) * n) : nullptr;
    if (n > 0) {
        SDL_memset(ents, 0, sizeof(sysent) * n);
    }
    uint32_t done = 0;
    for (; done < n && !r.fail; done++) {
        sysent *e = &ents[done];
        e->path = r_str(&r);
        e->face = r_u32(&r);
        e->size = r_u64(&r);
        e->mtime = (int64_t)r_u64(&r);
        e->legacy = r_str(&r);
        e->typo = r_str(&r);
        e->full = r_str(&r);
        e->ps = r_str(&r);
        uint32_t na = r_u32(&r);
        if (na > SYS_MAX_ALT) {
            r.fail = true;
            done++; /* the strings read so far belong to ents[done]: the cleanup below must free them */
            break;
        }
        if (na > 0) {
            /* nalt is set at once, so the entry frees exactly what it allocated, even when the read fails midway. */
            e->alt = embi_alloc(sizeof(char *) * na);
            SDL_memset(e->alt, 0, sizeof(char *) * na);
            e->nalt = na;
            for (uint32_t k = 0; k < na && !r.fail; k++) {
                e->alt[k] = r_str(&r);
            }
        }
        e->weight = r_u16(&r);
        e->width = r_u16(&r);
        e->style = r_u8(&r);
        e->flags = r_u8(&r);
        e->wlo = r_f32(&r);
        e->whi = r_f32(&r);
        e->slo = r_f32(&r);
        e->shi = r_f32(&r);
        bool empty = (e->flags & SYS_FLAG_EMPTY) != 0;
        if (empty) {
            if (e->face != SYS_FACE_NONE) {
                r.fail = true;
            }
        } else if (r_take(&r, EMBI_COVER_BYTES)) {
            e->cover = embi_alloc(EMBI_COVER_BYTES);
            SDL_memcpy(e->cover, r.p + r.at, EMBI_COVER_BYTES);
            r.at += EMBI_COVER_BYTES;
        } else {
            r.fail = true;
        }
        if (e->path == nullptr || (!empty && e->cover == nullptr)) {
            r.fail = true;
        }
        if (r.fail) {
            done++;
            break;
        }
    }
    if (r.fail || done != n) {
        for (uint32_t i = 0; i < done; i++) {
            sysent_clear(&ents[i]);
        }
        if (ents != nullptr) {
            embi_free(ents, sizeof(sysent) * n);
        }
        return false;
    }
    SDL_qsort(ents, n, sizeof(sysent), sent_cmp);
    *out = ents;
    *nout = n;
    return true;
}

static const char *cache_file(const embi_sysindex *x)
{
    return x->cache_name[0] != '\0' ? x->cache_name : SYS_CACHE_FILE;
}

/* Opens the user storage of the cache and waits (briefly) until it is ready. */
static SDL_Storage *cache_open(void)
{
    SDL_Storage *st = SDL_OpenUserStorage("ember", "fontcache", 0);
    if (st == nullptr) {
        return nullptr;
    }
    uint64_t t0 = SDL_GetTicksNS();
    while (!SDL_StorageReady(st) && ms_since(t0) < 2000.0) {
        SDL_Delay(1);
    }
    if (!SDL_StorageReady(st)) {
        SDL_CloseStorage(st);
        return nullptr;
    }
    return st;
}

static bool cache_load(SDL_Storage *st, const char *file, sysent **out, uint32_t *nout)
{
    uint64_t len = 0;
    if (!SDL_GetStorageFileSize(st, file, &len) || len == 0 || len > (uint64_t)SYS_TABLE_MAX * 64u) {
        return false;
    }
    uint8_t *buf = SDL_malloc((size_t)len);
    if (buf == nullptr) {
        return false;
    }
    bool ok = SDL_ReadStorageFile(st, file, buf, len) && cache_read(buf, (size_t)len, out, nout);
    SDL_free(buf);
    return ok;
}

static void cache_save(SDL_Storage *st, const char *file, const sysent *ents, uint32_t n)
{
    wbuf w = {nullptr, 0, 0, false};
    cache_write(&w, ents, n);
    if (w.p != nullptr && !w.fail) {
        SDL_WriteStorageFile(st, file, w.p, w.n);
        SDL_free(w.p);
    }
}

/* ------------------------------------------------------------------ rows (the name lookups) */

static int sysrow_cmp(const void *a, const void *b)
{
    const sysrow *x = a, *y = b;
    if (x->hash != y->hash) {
        return x->hash < y->hash ? -1 : 1;
    }
    return x->ent < y->ent ? -1 : x->ent > y->ent ? 1 : 0;
}

static void build_rows(embi_sysindex *x)
{
    uint32_t nr = 0, nl = 0;
    for (uint32_t i = 0; i < x->n; i++) {
        const sysent *e = &x->ents[i];
        nr += (e->typo != nullptr) + (e->legacy != nullptr) + e->nalt;
        nl += (e->full != nullptr) + (e->ps != nullptr);
    }
    x->rows = nr > 0 ? embi_alloc(sizeof(sysrow) * nr) : nullptr;
    x->lrows = nl > 0 ? embi_alloc(sizeof(sysrow) * nl) : nullptr;
    x->nrows = nr;
    x->nlrows = nl;
    uint32_t ir = 0, il = 0;
    for (uint32_t i = 0; i < x->n; i++) {
        const sysent *e = &x->ents[i];
        if (e->typo != nullptr) {
            x->rows[ir++] = (sysrow){fold_hash(e->typo, SDL_strlen(e->typo)), i, 0, 0};
        }
        if (e->legacy != nullptr) {
            x->rows[ir++] = (sysrow){fold_hash(e->legacy, SDL_strlen(e->legacy)), i, 1, 0};
        }
        for (uint32_t k = 0; k < e->nalt; k++) {
            x->rows[ir++] = (sysrow){fold_hash(e->alt[k], SDL_strlen(e->alt[k])), i, 2, k};
        }
        if (e->full != nullptr) {
            x->lrows[il++] = (sysrow){fold_hash(e->full, SDL_strlen(e->full)), i, 0, 0};
        }
        if (e->ps != nullptr) {
            x->lrows[il++] = (sysrow){fold_hash(e->ps, SDL_strlen(e->ps)), i, 1, 0};
        }
    }
    if (nr > 1) {
        SDL_qsort(x->rows, nr, sizeof(sysrow), sysrow_cmp);
    }
    if (nl > 1) {
        SDL_qsort(x->lrows, nl, sizeof(sysrow), sysrow_cmp);
    }
}

static uint32_t lower_bound(const sysrow *rows, uint32_t n, uint64_t h)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (rows[mid].hash < h) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static const char *family_row_name(const sysent *e, uint32_t tier, uint32_t idx)
{
    if (tier == 0) {
        return e->typo;
    }
    if (tier == 1) {
        return e->legacy;
    }
    return idx < e->nalt ? e->alt[idx] : nullptr;
}

static uint32_t add_unique(uint32_t *out, uint32_t cnt, uint32_t cap, uint32_t ent)
{
    for (uint32_t j = 0; j < cnt; j++) {
        if (out[j] == ent) {
            return cnt;
        }
    }
    if (cnt < cap) {
        out[cnt++] = ent;
    }
    return cnt;
}

/* ------------------------------------------------------------------ the job */

/* Frees the entries in a list (the moved ones are cleared, so only what remains is freed). */
static void entries_free(sysent *ents, uint32_t n, uint32_t cap)
{
    for (uint32_t i = 0; i < n; i++) {
        sysent_clear(&ents[i]);
    }
    if (ents != nullptr) {
        embi_free(ents, sizeof(sysent) * cap);
    }
}

static void index_free_parts(embi_sysindex *x)
{
    entries_free(x->ents, x->n, x->cap);
    x->ents = nullptr;
    x->n = x->cap = 0;
    if (x->rows != nullptr) {
        embi_free(x->rows, sizeof(sysrow) * x->nrows);
        x->rows = nullptr;
    }
    if (x->lrows != nullptr) {
        embi_free(x->lrows, sizeof(sysrow) * x->nlrows);
        x->lrows = nullptr;
    }
    x->nrows = x->nlrows = 0;
}

/* Builds the index: discovery, the cache, parsing of the changed files, the rows, then publication (ready). A cancelled
 * build frees its work and publishes nothing. */
static int sys_job(void *arg)
{
    embi_sysindex *x = arg;
    uint64_t t0 = SDL_GetTicksNS();
    if (x->opts.delay_ms > 0) {
        SDL_Delay(x->opts.delay_ms);
    }
    char **dirs = x->dirs;
    uint32_t nd = x->ndirs;
    bool own_dirs = false;
    uint32_t dirs_cap = 0;
    if (nd == 0) {
        nd = platform_dirs(&dirs, &dirs_cap);
        own_dirs = true;
    }

    flist files = {nullptr, 0, 0};
    walk_state ws = {&files, 0, x, nullptr};
    for (uint32_t i = 0; i < nd && SDL_GetAtomicInt(&x->cancel) == 0; i++) {
        SDL_EnumerateDirectory(dirs[i], walk_cb, &ws);
    }
    if (own_dirs) {
        for (uint32_t i = 0; i < nd; i++) {
            str_free(dirs[i]);
        }
        if (dirs != nullptr) {
            embi_free(dirs, sizeof(char *) * dirs_cap);
        }
    }
    if (files.n > 1) {
        SDL_qsort(files.v, files.n, sizeof(sfile), sfile_cmp);
    }
    /* A path listed twice (overlapping directories) is indexed once. */
    uint32_t uniq = 0;
    for (uint32_t i = 0; i < files.n; i++) {
        if (uniq > 0 && SDL_strcmp(files.v[uniq - 1].path, files.v[i].path) == 0) {
            str_free(files.v[i].path);
            continue;
        }
        files.v[uniq++] = files.v[i];
    }
    files.n = uniq;

    sysent *cache = nullptr;
    uint32_t ncache = 0;
    SDL_Storage *st = nullptr;
    bool cache_ok = false;
    if (x->opts.cache && SDL_GetAtomicInt(&x->cancel) == 0) {
        st = cache_open();
        cache_ok = st != nullptr && cache_load(st, cache_file(x), &cache, &ncache);
    }

    /* The cache's paths, kept apart: a moved cache entry loses its strings, but the search below goes on. */
    const char **cpath = ncache > 0 ? embi_alloc(sizeof(char *) * ncache) : nullptr;
    for (uint32_t k = 0; k < ncache; k++) {
        cpath[k] = cache[k].path;
    }
    sysent *ents = nullptr;
    uint32_t n = 0, cap = 0, reparsed = 0;
    bool cancelled = false;
    for (uint32_t i = 0; i < files.n; i++) {
        if (SDL_GetAtomicInt(&x->cancel) != 0) {
            cancelled = true;
            break;
        }
        sfile *f = &files.v[i];
        /* The cached entries of this path, contiguous in the cache (sorted by path). */
        uint32_t lo = 0, hi = ncache;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (SDL_strcmp(cpath[mid], f->path) < 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        uint32_t end = lo;
        while (end < ncache && SDL_strcmp(cpath[end], f->path) == 0) {
            end++;
        }
        bool hit = end > lo && cache[lo].size == f->size && cache[lo].mtime == f->mtime;
        if (hit) {
            for (uint32_t k = lo; k < end; k++) {
                if (n == cap) {
                    uint32_t ncap = cap == 0 ? 64u : cap * 2u;
                    ents = embi_realloc(ents, sizeof(sysent) * cap, sizeof(sysent) * ncap);
                    cap = ncap;
                }
                ents[n++] = cache[k];
                SDL_memset(&cache[k], 0, sizeof cache[k]); /* moved: the cache no longer owns it */
            }
        } else {
            reparsed++;
            if (parse_file(f->path, f->size, f->mtime, &ents, &n, &cap) == 0) {
                /* No face: a zero-face record keeps the file's key in the cache, so a warm start does not parse it
                 * again. It is dropped from the index below. */
                if (n == cap) {
                    uint32_t ncap = cap == 0 ? 64u : cap * 2u;
                    ents = embi_realloc(ents, sizeof(sysent) * cap, sizeof(sysent) * ncap);
                    cap = ncap;
                }
                sysent *z = &ents[n++];
                SDL_memset(z, 0, sizeof *z);
                z->path = str_dup_cstr(f->path);
                z->face = SYS_FACE_NONE;
                z->size = f->size;
                z->mtime = f->mtime;
                z->flags = SYS_FLAG_EMPTY;
            }
        }
    }
    if (cpath != nullptr) {
        embi_free(cpath, sizeof(char *) * ncache);
    }
    if (cancelled || SDL_GetAtomicInt(&x->cancel) != 0) {
        entries_free(ents, n, cap);
        entries_free(cache, ncache, ncache);
        for (uint32_t i = 0; i < files.n; i++) {
            str_free(files.v[i].path);
        }
        embi_free(files.v, sizeof(sfile) * files.cap);
        if (st != nullptr) {
            SDL_CloseStorage(st);
        }
        return 0;
    }

    if (n > 1) {
        SDL_qsort(ents, n, sizeof(sysent), sent_cmp);
    }
    bool same_as_cache = cache_ok && reparsed == 0 && n == ncache;
    if (st != nullptr && !same_as_cache) {
        cache_save(st, cache_file(x), ents, n);
    }
    if (st != nullptr) {
        SDL_CloseStorage(st);
    }
    entries_free(cache, ncache, ncache);
    for (uint32_t i = 0; i < files.n; i++) {
        str_free(files.v[i].path);
    }
    embi_free(files.v, sizeof(sfile) * files.cap);
    /* The zero-face records were for the cache only. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        if ((ents[i].flags & SYS_FLAG_EMPTY) != 0) {
            sysent_clear(&ents[i]);
            continue;
        }
        if (kept != i) {
            ents[kept] = ents[i];
        }
        kept++;
    }
    n = kept;

    /* Publish: the rows and the memory estimate, then the ready flag (a barrier). */
    x->ents = ents;
    x->n = n;
    x->cap = cap;
    x->files = files.n;
    x->reparsed = reparsed;
    x->warm = cache_ok && reparsed == 0;
    build_rows(x);
    uint64_t bytes = sizeof(sysent) * (uint64_t)cap + sizeof(sysrow) * (uint64_t)(x->nrows + x->nlrows);
    for (uint32_t i = 0; i < n; i++) {
        bytes += EMBI_COVER_BYTES + SDL_strlen(ents[i].path) + 1;
    }
    x->index_bytes = bytes;
    x->index_ms = ms_since(t0);
    x->epoch = 1;
    SDL_SetAtomicInt(&x->ready, 1);
    return 0;
}

/* ------------------------------------------------------------------ public (internal) API */

/* The test directories (embi_sysfont_set_dirs): copies, read on the thread that starts an index. */
static char *g_test_dirs[SYS_MAX_DIRS];
static uint32_t g_test_ndirs;
static uint32_t g_test_delay_ms;

void embi_sysfont_set_dirs(const char *const *dirs, uint32_t n, uint32_t delay_ms)
{
    g_test_delay_ms = delay_ms;
    for (uint32_t i = 0; i < g_test_ndirs; i++) {
        str_free(g_test_dirs[i]);
        g_test_dirs[i] = nullptr;
    }
    g_test_ndirs = 0;
    if (dirs == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < n && i < SYS_MAX_DIRS; i++) {
        g_test_dirs[g_test_ndirs++] = str_dup_cstr(dirs[i]);
    }
}

bool embi_sys_start(embi_fontdb *db, const embi_sys_opts *opts)
{
    if (db == nullptr) {
        return false;
    }
    if (db->sys != nullptr) {
        return true;
    }
    embi_sysindex *x = embi_calloc(1, sizeof *x);
    if (opts != nullptr) {
        x->opts = *opts;
    }
    if (opts != nullptr && opts->ndirs > 0 && opts->dirs != nullptr) {
        uint32_t nd = opts->ndirs > SYS_MAX_DIRS ? SYS_MAX_DIRS : opts->ndirs;
        x->dirs = embi_alloc(sizeof(char *) * nd);
        for (uint32_t i = 0; i < nd; i++) {
            x->dirs[i] = str_dup_cstr(opts->dirs[i]);
        }
        x->ndirs = nd;
    }
    x->opts.dirs = nullptr;
    x->opts.ndirs = 0;
    if (x->ndirs == 0 && g_test_ndirs > 0) {
        /* The test directories replace the platform's; their index has its own cache file, not the user's. */
        x->dirs = embi_alloc(sizeof(char *) * g_test_ndirs);
        for (uint32_t i = 0; i < g_test_ndirs; i++) {
            x->dirs[i] = str_dup_cstr(g_test_dirs[i]);
        }
        x->ndirs = g_test_ndirs;
        x->opts.delay_ms = g_test_delay_ms;
        SDL_strlcpy(x->cache_name, "index-testdirs.bin", sizeof x->cache_name);
    }
    if (opts != nullptr && opts->cache_name != nullptr) {
        SDL_strlcpy(x->cache_name, opts->cache_name, sizeof x->cache_name);
    }
    x->opts.cache_name = nullptr;
    SDL_SetAtomicInt(&x->ready, 0);
    SDL_SetAtomicInt(&x->cancel, 0);
    db->sys = x;
    if (x->opts.sync) {
        sys_job(x);
        return true;
    }
    x->thread = SDL_CreateThread(sys_job, "ember-fonts", x);
    if (x->thread == nullptr) {
        /* No thread can be made: the index is built here, once (a slower start, the same result). */
        sys_job(x);
    }
    return true;
}

void embi_sys_stop(embi_fontdb *db)
{
    if (db == nullptr || db->sys == nullptr) {
        return;
    }
    embi_sysindex *x = db->sys;
    SDL_SetAtomicInt(&x->cancel, 1);
    if (x->thread != nullptr) {
        SDL_WaitThread(x->thread, nullptr);
        x->thread = nullptr;
    }
    db->sys = nullptr;
    embi_fontdb_cover_clear(db);
    for (uint32_t i = 0; i < x->n; i++) {
        if (x->ents[i].loaded != nullptr) {
            embi_face_release(x->ents[i].loaded);
            x->ents[i].loaded = nullptr;
        }
    }
    index_free_parts(x);
    for (uint32_t i = 0; i < x->ndirs; i++) {
        str_free(x->dirs[i]);
    }
    if (x->dirs != nullptr) {
        embi_free(x->dirs, sizeof(char *) * x->ndirs);
    }
    embi_free(x, sizeof *x);
}

/* The index fields (entries, rows, counts) are the job thread's until the ready flag publishes them: every accessor
 * reads the flag first. */
static bool x_ready(const embi_sysindex *x)
{
    return x != nullptr && SDL_GetAtomicInt((SDL_AtomicInt *)&x->ready) != 0;
}

bool embi_sys_ready(const embi_fontdb *db)
{
    return db != nullptr && db->sys != nullptr && SDL_GetAtomicInt((SDL_AtomicInt *)&db->sys->ready) != 0;
}

void embi_sys_stats_get(const embi_fontdb *db, embi_sys_stats *out)
{
    if (out == nullptr) {
        return;
    }
    SDL_memset(out, 0, sizeof *out);
    if (db == nullptr || db->sys == nullptr) {
        return;
    }
    const embi_sysindex *x = db->sys;
    /* The loaded-face accounting belongs to the main thread; everything else is the job's until ready publishes it. */
    out->faces_loaded = x->loaded;
    out->font_bytes = x->loaded_bytes;
    out->ready = SDL_GetAtomicInt((SDL_AtomicInt *)&x->ready) != 0;
    if (!out->ready) {
        return;
    }
    out->warm = x->warm;
    out->files = x->files;
    out->entries = x->n;
    out->reparsed = x->reparsed;
    out->index_bytes = x->index_bytes;
    out->index_ms = x->index_ms;
    out->epoch = x->epoch;
}

uint64_t embi_fontdb_sys_epoch(const embi_fontdb *db)
{
    return embi_sys_ready(db) ? db->sys->epoch : 0;
}

uint32_t embi_sys_count(const embi_sysindex *x)
{
    return x_ready(x) ? x->n : 0;
}

bool embi_sys_meta(const embi_sysindex *x, uint32_t i, embi_fmeta *m, bool *color)
{
    if (!x_ready(x) || i >= x->n) {
        return false;
    }
    const sysent *e = &x->ents[i];
    if (m != nullptr) {
        m->wlo = e->wlo;
        m->whi = e->whi;
        m->slo = e->slo;
        m->shi = e->shi;
        m->style = e->style;
    }
    if (color != nullptr) {
        *color = (e->flags & SYS_FLAG_COLOR) != 0;
    }
    return true;
}

bool embi_sys_block(const embi_sysindex *x, uint32_t i, uint32_t cp)
{
    if (!x_ready(x) || i >= x->n || cp > 0x10FFFFu) {
        return false;
    }
    uint32_t b = cp >> 8;
    return (x->ents[i].cover[b >> 3] >> (b & 7u)) & 1u;
}

uint32_t embi_sys_family(const embi_sysindex *x, embi_str name, uint32_t *out, uint32_t cap)
{
    if (!x_ready(x) || out == nullptr || name.len == 0 || x->nrows == 0) {
        return 0;
    }
    uint64_t h = fold_hash(name.ptr, name.len);
    uint32_t best = 0xFFFFFFFFu, cnt = 0;
    for (uint32_t i = lower_bound(x->rows, x->nrows, h); i < x->nrows && x->rows[i].hash == h; i++) {
        const sysrow *r = &x->rows[i];
        const char *s = family_row_name(&x->ents[r->ent], r->tier, r->idx);
        if (s == nullptr || !ascii_ieq(s, SDL_strlen(s), name.ptr, name.len)) {
            continue;
        }
        if (r->tier < best) {
            best = r->tier;
            cnt = 0;
        }
        if (r->tier == best) {
            cnt = add_unique(out, cnt, cap, r->ent);
        }
    }
    return cnt;
}

uint32_t embi_sys_local(const embi_sysindex *x, embi_str name, uint32_t *out, uint32_t cap)
{
    if (!x_ready(x) || out == nullptr || name.len == 0 || x->nlrows == 0) {
        return 0;
    }
    uint64_t h = fold_hash(name.ptr, name.len);
    uint32_t cnt = 0;
    for (uint32_t i = lower_bound(x->lrows, x->nlrows, h); i < x->nlrows && x->lrows[i].hash == h; i++) {
        const sysrow *r = &x->lrows[i];
        const sysent *e = &x->ents[r->ent];
        const char *s = r->tier == 0 ? e->full : e->ps;
        if (s != nullptr && ascii_ieq(s, SDL_strlen(s), name.ptr, name.len)) {
            cnt = add_unique(out, cnt, cap, r->ent);
        }
    }
    return cnt;
}

const char *embi_sys_path(const embi_sysindex *x, uint32_t i, uint32_t *face_index)
{
    if (!x_ready(x) || i >= x->n) {
        return nullptr;
    }
    if (face_index != nullptr) {
        *face_index = x->ents[i].face;
    }
    return x->ents[i].path;
}

bool embi_sys_read(const embi_sysindex *x, uint32_t i, uint8_t **data, size_t *len)
{
    if (!x_ready(x) || i >= x->n || data == nullptr || len == nullptr) {
        return false;
    }
    SDL_IOStream *io = SDL_IOFromFile(x->ents[i].path, "rb");
    if (io == nullptr) {
        return false;
    }
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0 || size > (Sint64)SYS_TABLE_MAX * 64) {
        SDL_CloseIO(io);
        return false;
    }
    uint8_t *buf = SDL_malloc((size_t)size);
    bool ok = buf != nullptr && SDL_ReadIO(io, buf, (size_t)size) == (size_t)size;
    SDL_CloseIO(io);
    if (!ok) {
        SDL_free(buf);
        return false;
    }
    *data = buf;
    *len = (size_t)size;
    return true;
}

embi_face **embi_sys_face_slot(embi_sysindex *x, uint32_t i)
{
    if (!x_ready(x) || i >= x->n) {
        return nullptr;
    }
    return &x->ents[i].loaded;
}

bool embi_sys_bad(const embi_sysindex *x, uint32_t i)
{
    return x_ready(x) && i < x->n && x->ents[i].bad;
}

void embi_sys_mark_bad(embi_sysindex *x, uint32_t i)
{
    if (x_ready(x) && i < x->n) {
        x->ents[i].bad = true;
    }
}

void embi_sys_loaded(embi_sysindex *x, int32_t faces, int64_t bytes)
{
    if (x == nullptr) {
        return;
    }
    x->loaded = (uint32_t)((int32_t)x->loaded + faces);
    x->loaded_bytes = (uint64_t)((int64_t)x->loaded_bytes + bytes);
}

int embi_sys_probe(const embi_sysindex *x, uint32_t i, uint32_t cp)
{
    if (!x_ready(x) || i >= x->n) {
        return -1;
    }
    const sysent *e = &x->ents[i];
    SDL_IOStream *io = SDL_IOFromFile(e->path, "rb");
    if (io == nullptr) {
        return -1;
    }
    Sint64 size = SDL_GetIOSize(io);
    fsrc src;
    SDL_memset(&src, 0, sizeof src);
    src.io = io;
    src.n = size > 0 ? (uint64_t)size : 0;
    int r = -1;
    uint8_t magic[4];
    if (fsrc_read(&src, 0, magic, 4) && !(magic[0] == 'w' && magic[1] == 'O' && magic[2] == 'F')) {
        /* WOFF and WOFF2 keep their cmap compressed: unknown, the caller loads the face. */
        uint64_t base = 0;
        bool ok = true;
        if (SDL_memcmp(magic, "ttcf", 4) == 0) {
            uint8_t ob[4];
            ok = fsrc_read(&src, 12 + 4ull * e->face, ob, 4);
            base = ok ? be32(ob) : 0;
        }
        sdir d;
        if (ok && read_dir(&src, base, &d)) {
            if (!d.cmap.has) {
                r = 0; /* no cmap: no code point maps */
            } else {
                uint8_t *tb = fsrc_table(&src, d.cmap.off, d.cmap.len);
                if (tb != nullptr) {
                    r = cmap_maps(tb, d.cmap.len, cp) ? 1 : 0;
                    SDL_free(tb);
                }
            }
        }
    }
    SDL_CloseIO(io);
    return r;
}
