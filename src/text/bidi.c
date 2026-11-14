/*
 * Bidi (R2c): SheenBidi (UAX #9) for paragraph levels and the visual order of a line.
 *
 * SheenBidi allocates through its default allocator. embi_bidi_init installs one backed by SDL
 * (SDL_malloc, SDL_realloc, SDL_free), once, under SDL_InitState. SheenBidi frees without a size,
 * so these blocks are not part of the embi_mem_live accounting.
 *
 * Cost. A layout whose levels are all zero (plain LTR text) keeps no SheenBidi objects: its
 * lines are already in visual order and line.c skips this file. Otherwise the layout keeps one
 * SBAlgorithm and one SBParagraph per paragraph (embi_bidi_cache), so a line costs only
 * SBParagraphCreateLine.
 */

#include "text/text_int.h"

#include <SheenBidi/SheenBidi.h>

#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_stdinc.h>

struct embi_bidi_cache {
    SBAlgorithmRef alg;
    SBParagraphRef *paras; /* one per paragraph of the layout, in order */
    char *buf;             /* the text with a strong character after it, when the analysis ran over one (it keeps the algorithm's bytes) */
    uint32_t buf_len;
    uint32_t n;
};

static void *sb_allocate(SBUInteger size, void *info)
{
    (void)info;
    return SDL_malloc((size_t)size);
}

static void *sb_reallocate(void *p, SBUInteger size, void *info)
{
    (void)info;
    return SDL_realloc(p, (size_t)size);
}

static void sb_deallocate(void *p, void *info)
{
    (void)info;
    SDL_free(p);
}

void embi_bidi_init(void)
{
    static SDL_InitState state;
    if (!SDL_ShouldInit(&state)) {
        return;
    }
    static const SBAllocatorProtocol protocol = {
        .allocateBlock = sb_allocate,
        .reallocateBlock = sb_reallocate,
        .deallocateBlock = sb_deallocate,
    };
    bool ok = false;
    SBAllocatorRef alloc = SBAllocatorCreate(&protocol, nullptr);
    if (alloc != nullptr) {
        SBAllocatorSetDefault(alloc);
        ok = true;
    }
    SDL_SetInitialized(&state, ok);
}

static SBLevel base_level_for(uint8_t dir)
{
    if (dir == EMBI_DIR_RTL) {
        return 1;
    }
    if (dir == EMBI_DIR_LTR) {
        return 0;
    }
    return SBLevelDefaultLTR;
}

/* Whether the bytes [off, off + n) hold a strong character (L, R or AL). */
static bool has_strong(SBAlgorithmRef alg, uint32_t off, uint32_t n)
{
    const SBBidiType *t = SBAlgorithmGetBidiTypesPtr(alg);
    for (uint32_t i = 0; t != nullptr && i < n; i++) {
        if (SBBidiTypeIsStrong(t[off + i])) {
            return true;
        }
    }
    return false;
}

void embi_bidi_cache_free(embi_bidi_cache *c)
{
    if (c == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < c->n; i++) {
        SBParagraphRelease(c->paras[i]);
    }
    embi_free(c->paras, sizeof(SBParagraphRef) * c->n);
    if (c->alg != nullptr) {
        SBAlgorithmRelease(c->alg);
    }
    if (c->buf != nullptr) {
        embi_free(c->buf, c->buf_len);
    }
    embi_free(c, sizeof *c);
}

bool embi_bidi_analyze(const char *text, uint32_t len, uint8_t dir, bool after, embi_para_vec *paras,
                       uint8_t *levels, embi_bidi_cache **cache)
{
    embi_bidi_init();
    *cache = nullptr;
    if (len == 0) {
        return true;
    }
    /* after: a strong character of the other direction follows the text (U+200E or U+200F, three bytes), which the
     * analysis sees as the next character of the paragraph; it is not part of the result. */
    uint32_t tot = len;
    char *buf = nullptr;
    if (after && (dir == EMBI_DIR_LTR || dir == EMBI_DIR_RTL)) {
        buf = embi_alloc((size_t)len + 3u);
        if (buf == nullptr) {
            return false;
        }
        SDL_memcpy(buf, text, len);
        SDL_memcpy(buf + len, dir == EMBI_DIR_RTL ? "\342\200\216" : "\342\200\217", 3);
        text = buf;
        tot = len + 3u;
    }
    SBCodepointSequence seq = {SBStringEncodingUTF8, text, tot};
    SBAlgorithmRef alg = SBAlgorithmCreate(&seq);
    if (alg == nullptr) {
        embi_free(buf, buf != nullptr ? (size_t)len + 3u : 0);
        return false;
    }
    EMBI_VEC(SBParagraphRef) pl = {0};
    bool any = false;
    bool ok = true;
    uint32_t off = 0;
    while (off < len) {
        SBUInteger actual = 0, sep = 0;
        SBAlgorithmGetParagraphBoundary(alg, off, tot - off, &actual, &sep);
        if (actual == 0 || actual > tot - off) {
            actual = tot - off;
        }
        /* A separator is at most the paragraph's length. Separator-only paragraphs (blank lines)
         * then have begin == end and no bytes of their own. */
        if (sep > actual) {
            sep = actual;
        }
        SBParagraphRef para = SBAlgorithmCreateParagraph(alg, off, actual, base_level_for(dir));
        if (para == nullptr) {
            ok = false;
            break;
        }
        const bool neutral = dir == EMBI_DIR_AUTO && !has_strong(alg, off, (uint32_t)actual);
        if (neutral && pl.len > 0) {
            /* A paragraph of weak and neutral characters only takes the direction of the one before it (CSS Text 3
             * section 7.2, unicode-bidi: plaintext). */
            SBParagraphRelease(para);
            para = SBAlgorithmCreateParagraph(alg, off, actual, (SBLevel)(paras->data[paras->len - 1].level & 1u));
            if (para == nullptr) {
                ok = false;
                break;
            }
        }
        const SBLevel *lv = SBParagraphGetLevelsPtr(para);
        for (uint32_t i = 0; i < actual && off + i < len; i++) {
            levels[off + i] = lv[i];
            any = any || lv[i] != 0;
        }
        embi_text_para p;
        p.begin = off;
        p.end = off + (uint32_t)(actual - sep);
        if (p.end > len) {
            p.end = len;
        }
        p.level = (uint8_t)SBParagraphGetBaseLevel(para);
        p.neutral = neutral && pl.len == 0;
        any = any || p.level != 0;
        embi_vec_push(paras, p);
        embi_vec_push(&pl, para);
        off += (uint32_t)actual;
    }
    if (!ok || !any) {
        for (uint32_t i = 0; i < pl.len; i++) {
            SBParagraphRelease(pl.data[i]);
        }
        embi_vec_free(&pl);
        SBAlgorithmRelease(alg);
        embi_free(buf, buf != nullptr ? (size_t)len + 3u : 0);
        return ok;
    }
    embi_bidi_cache *c = embi_calloc(1, sizeof *c);
    c->alg = alg;
    c->buf = buf;
    c->buf_len = buf != nullptr ? len + 3u : 0;
    c->n = pl.len;
    c->paras = embi_alloc(sizeof(SBParagraphRef) * (pl.len > 0 ? pl.len : 1));
    for (uint32_t i = 0; i < pl.len; i++) {
        c->paras[i] = pl.data[i];
    }
    embi_vec_free(&pl);
    *cache = c;
    return true;
}

bool embi_bidi_line_runs(const embi_bidi_cache *c, uint32_t para_index, uint32_t start,
                         uint32_t end, embi_bidi_run_vec *runs)
{
    runs->len = 0;
    if (end <= start) {
        return true;
    }
    if (c == nullptr || para_index >= c->n) {
        return false;
    }
    /* A line that ends the text is made over the strong character after it too, so that its trailing spaces keep the level
     * the neighbouring text gave them (rule L1 would take them to the paragraph's level); the character is cut off the runs. */
    const uint32_t text_end = c->buf != nullptr ? c->buf_len - 3u : 0u;
    const bool ext = c->buf != nullptr && end == text_end;
    /* Offsets are in the source string (SheenBidi convention), not relative to the paragraph. */
    SBLineRef line = SBParagraphCreateLine(c->paras[para_index], start, end - start + (ext ? 3u : 0u));
    if (line == nullptr) {
        return false;
    }
    SBUInteger count = SBLineGetRunCount(line);
    const SBRun *rs = SBLineGetRunsPtr(line);
    bool ok = true;
    for (SBUInteger i = 0; i < count && ok; i++) {
        embi_bidi_run r;
        r.begin = (uint32_t)rs[i].offset;
        r.len = (uint32_t)rs[i].length;
        r.level = (uint8_t)rs[i].level;
        if (ext) {
            if (r.begin >= text_end) {
                continue;
            }
            if (r.begin + r.len > text_end) {
                r.len = text_end - r.begin;
            }
        }
        embi_vec_push(runs, r);
    }
    SBLineRelease(line);
    return ok;
}
