/*
 * Layout of very long runs (W3q). One unbreakable word or one nowrap run of a megabyte must lay out in time
 * roughly linear in its length: the inline layout used to measure the tail of the run for each break
 * candidate, which took half a minute for 200 KB. The text box splitting at EMBI_TEXT_MAX (box.c text_boxes)
 * is tested here too, with the limit lowered through embi_lay_set_text_max so a few bytes reach it.
 */
#include "layout_harness.h"

#include <SDL3/SDL_timer.h>

/* The time bounds are generous (a linear layout needs a few hundred ms); a sanitizer slows it down. */
#ifdef EMB_TEST_ASAN
#define LONG_RUN_MS 60000.0
#else
#define LONG_RUN_MS 15000.0
#endif

/* The length of the long runs: a megabyte, or EMBER_LONG_RUN_BYTES for a profile of a smaller one. */
static size_t run_len(void)
{
    const char *e = SDL_getenv("EMBER_LONG_RUN_BYTES");
    int v = e != nullptr ? SDL_atoi(e) : 0;
    return v > 0 ? (size_t)v : (size_t)1u << 20;
}

static double now_ms(void)
{
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/* A document with one paragraph holding text (n bytes), in a 300px wide box. */
static lh make_para(const char *style, const char *text, size_t n)
{
    static const char head[] = "<!DOCTYPE html><html><body><p id=p>";
    static const char tail[] = "</p></body></html>";
    size_t total = sizeof head - 1 + n + sizeof tail - 1;
    char *html = SDL_malloc(total + 1);
    memcpy(html, head, sizeof head - 1);
    memcpy(html + sizeof head - 1, text, n);
    memcpy(html + sizeof head - 1 + n, tail, sizeof tail);
    lh h = lh_make_n(html, total, style);
    SDL_free(html);
    return h;
}

/* The widest text fragment extent and the number of text fragments below f. */
static void sum_text(const embi_frag *f, uint32_t *texts, uint32_t *bytes)
{
    for (; f != nullptr; f = f->next) {
        if (f->kind == EMBI_FK_TEXT) {
            (*texts)++;
            *bytes += embi_frag_tend(f) - embi_frag_tbegin(f);
        }
        sum_text(embi_frag_first(f), texts, bytes);
    }
}

EMB_TEST(a_one_megabyte_word_lays_out_in_linear_time)
{
    const size_t n = run_len();
    char *w = SDL_malloc(n);
    for (size_t i = 0; i < n; i++) {
        w[i] = (char)('a' + (i * 7 + i / 13) % 26);
    }
    lh warm = lh_make("<p>warm up the process wide tables</p>", "");
    lh_free(&warm);
    size_t live0 = embi_mem_live();
    double t0 = now_ms();
    lh h = make_para("p { width: 300px; font: 16px sans-serif }", w, n);
    double dt = now_ms() - t0;
    EMB_CHECK(dt < LONG_RUN_MS);
    uint32_t texts = 0, bytes = 0;
    sum_text(embi_layout_root(h.eng, h.doc), &texts, &bytes);
    EMB_CHECK(texts >= 1);
    EMB_CHECK_EQ_INT(bytes, (uint32_t)n); /* every byte is drawn once */
    /* The unchanged relayout keeps the box tree and is just as bounded. */
    t0 = now_ms();
    (void)embi_layout_document(h.eng, h.doc);
    EMB_CHECK(now_ms() - t0 < LONG_RUN_MS);
    lh_free(&h);
    SDL_free(w);
    EMB_CHECK_EQ_INT((long long)embi_mem_live(), (long long)live0);
}

EMB_TEST(a_one_megabyte_word_that_may_break_anywhere_lays_out_in_linear_time)
{
    const size_t n = run_len();
    char *w = SDL_malloc(n);
    for (size_t i = 0; i < n; i++) {
        w[i] = (char)('a' + (i * 5 + i / 11) % 26);
    }
    double t0 = now_ms();
    lh h = make_para("p { width: 300px; font: 16px sans-serif; overflow-wrap: anywhere }", w, n);
    EMB_CHECK(now_ms() - t0 < LONG_RUN_MS);
    uint32_t texts = 0, bytes = 0;
    sum_text(embi_layout_root(h.eng, h.doc), &texts, &bytes);
    EMB_CHECK(texts > 100); /* many lines */
    EMB_CHECK_EQ_INT(bytes, (uint32_t)n);
    lh_free(&h);
    SDL_free(w);
}

EMB_TEST(a_one_megabyte_nowrap_run_lays_out_in_linear_time)
{
    const size_t n = run_len();
    char *w = SDL_malloc(n);
    for (size_t i = 0; i < n; i++) {
        w[i] = i % 7 == 6 ? ' ' : (char)('a' + (i * 3 + i / 5) % 26);
    }
    double t0 = now_ms();
    lh h = make_para("p { width: 300px; font: 16px sans-serif; white-space: nowrap }", w, n);
    EMB_CHECK(now_ms() - t0 < LONG_RUN_MS);
    uint32_t texts = 0, bytes = 0;
    sum_text(embi_layout_root(h.eng, h.doc), &texts, &bytes);
    EMB_CHECK(texts >= 1);
    EMB_CHECK(bytes > (uint32_t)(n - n / 7 - 2)); /* the spaces at line ends may hang, none are lost mid-line */
    lh_free(&h);
    SDL_free(w);
}

/* ------------------------------------------------------------------ the EMBI_TEXT_MAX split */

/* The text boxes of the paragraph (the box tree below b), in order. */
static void collect_texts(const embi_box *b, const embi_box **out, uint32_t *n, uint32_t cap)
{
    for (const embi_box *k = embi_box_first(b); k != nullptr; k = k->next) {
        if (k->kind == EMBI_BK_TEXT && *n < cap) {
            out[(*n)++] = k;
        }
        if (!embi_box_is_text(k)) {
            collect_texts(k, out, n, cap);
        }
    }
}

/* The box of element el, from the document. */
static const embi_box *box_of_id(const lh *h, const char *id)
{
    embi_element *el = embi_get_element_by_id(h->doc, (embi_str){id, (uint32_t)strlen(id)});
    return el != nullptr ? (const embi_box *)el->box : nullptr;
}

/* The text box limit is lowered with embi_lay_set_text_max so that a few bytes reach it (0 restores it). */
EMB_TEST(text_longer_than_the_box_limit_splits_at_character_boundaries)
{
    embi_lay_set_text_max(10);
    /* "aaaa" + 4 two-byte letters + 3-byte + 4-byte characters: a 10-byte limit falls inside characters. */
    static const char txt[] = "aaaaaaaaa\xC3\xA9\xC3\xA9\xC3\xA9\xE2\x82\xAC\xE2\x82\xAC\xF0\x9F\x98\x80\xF0\x9F\x98\x80zz";
    lh h = make_para("p { width: 1000px; font: 16px sans-serif }", txt, sizeof txt - 1);
    const embi_box *p = box_of_id(&h, "p");
    EMB_CHECK(p != nullptr);
    const embi_box *boxes[32];
    uint32_t n = 0;
    collect_texts(p, boxes, &n, 32);
    EMB_CHECK(n >= 4);
    uint32_t total = 0;
    char joined[128];
    for (uint32_t i = 0; i < n; i++) {
        EMB_CHECK(boxes[i]->text_len > 0);
        EMB_CHECK(boxes[i]->text_len <= 10);
        /* Never split inside a UTF-8 sequence: a box starts and ends on a character boundary. */
        EMB_CHECK(((uint8_t)boxes[i]->text[0] & 0xC0) != 0x80);
        memcpy(joined + total, boxes[i]->text, boxes[i]->text_len);
        total += boxes[i]->text_len;
    }
    EMB_CHECK_EQ_INT(total, (uint32_t)(sizeof txt - 1));
    EMB_CHECK(memcmp(joined, txt, total) == 0);
    lh_free(&h);
    embi_lay_set_text_max(0);
}

EMB_TEST(a_limit_smaller_than_one_character_still_makes_progress)
{
    /* With a limit of 2 bytes a 4-byte character cannot fit: the split backs off to nothing (n == 0) and cuts at
     * the limit anyway, so the text is neither lost nor looped over. */
    embi_lay_set_text_max(2);
    static const char txt[] = "\xF0\x9F\x98\x80\xF0\x9F\x98\x80" "ab";
    lh h = make_para("p { width: 1000px; font: 16px sans-serif }", txt, sizeof txt - 1);
    const embi_box *p = box_of_id(&h, "p");
    EMB_CHECK(p != nullptr);
    const embi_box *boxes[32];
    uint32_t n = 0;
    collect_texts(p, boxes, &n, 32);
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        EMB_CHECK(boxes[i]->text_len > 0);
        EMB_CHECK(boxes[i]->text_len <= 2);
        total += boxes[i]->text_len;
    }
    EMB_CHECK_EQ_INT(total, (uint32_t)(sizeof txt - 1));
    EMB_CHECK_EQ_INT(n, 5); /* 10 bytes in boxes of 2 */
    lh_free(&h);
    embi_lay_set_text_max(0);
}

EMB_TEST(split_text_boxes_lay_out_like_one_unbroken_line)
{
    /* The same text with and without the split gives the same line count and the same total advance. */
    static const char txt[] = "split paragraphs of text that wrap onto several lines in a narrow box";
    lh a = make_para("p { width: 120px; font: 16px sans-serif }", txt, sizeof txt - 1);
    embi_lay_set_text_max(8);
    lh b = make_para("p { width: 120px; font: 16px sans-serif }", txt, sizeof txt - 1);
    embi_lay_set_text_max(0);
    const embi_box *pa = box_of_id(&a, "p"), *pb = box_of_id(&b, "p");
    EMB_CHECK(pa != nullptr && pb != nullptr);
    EMB_CHECK(pa->frag != nullptr && pb->frag != nullptr);
    EMB_CHECK(pa->frag->h == pb->frag->h);
    uint32_t ta = 0, ba = 0, tb = 0, bb = 0;
    sum_text(embi_layout_root(a.eng, a.doc), &ta, &ba);
    sum_text(embi_layout_root(b.eng, b.doc), &tb, &bb);
    EMB_CHECK_EQ_INT(ba, bb);
    EMB_CHECK(tb >= ta); /* the split adds fragments, never removes text */
    /* Line by line: the same number of lines, each as wide (within the kerning at the cuts) and as tall. */
    const embi_frag *la = embi_frag_first(pa->frag), *lb = embi_frag_first(pb->frag);
    uint32_t lines = 0;
    while (la != nullptr && lb != nullptr) {
        EMB_CHECK_EQ_INT(la->kind, lb->kind);
        EMB_CHECK_EQ_INT(la->kind, EMBI_FK_LINE);
        float dw = la->w - lb->w;
        EMB_CHECK(dw < 2.0f && dw > -2.0f);
        EMB_CHECK(la->h == lb->h);
        EMB_CHECK(la->y == lb->y);
        uint32_t t1 = 0, b1 = 0, t2 = 0, b2 = 0;
        sum_text(embi_frag_first(la), &t1, &b1);
        sum_text(embi_frag_first(lb), &t2, &b2);
        EMB_CHECK_EQ_INT(b1, b2); /* the same bytes on the line */
        lines++;
        la = la->next;
        lb = lb->next;
    }
    EMB_CHECK(la == nullptr && lb == nullptr); /* the same number of lines */
    EMB_CHECK(lines >= 3);
    lh_free(&a);
    lh_free(&b);
}

EMB_TEST_MAIN()
