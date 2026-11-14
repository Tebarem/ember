#include "emb_gold.h"

/* Synthetic buffers only: nothing here touches the disk or the GPU. */

static void fill_px(uint8_t *px, int n, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int i = 0; i < n; i++) {
        px[i * 4 + 0] = r;
        px[i * 4 + 1] = g;
        px[i * 4 + 2] = b;
        px[i * 4 + 3] = a;
    }
}

EMB_TEST(gold_identical_buffers_pass)
{
    uint8_t a[16 * 16 * 4];
    uint8_t b[16 * 16 * 4];
    fill_px(a, 16 * 16, 10, 200, 33, 255);
    fill_px(b, 16 * 16, 10, 200, 33, 255);
    emb_gold_report r = emb_gold_compare(a, b, 16, 16);
    EMB_CHECK(emb_gold_report_pass(&r));
    EMB_CHECK_EQ_INT(r.mismatched, 0);
    EMB_CHECK_EQ_INT(r.max_diff, 0);
    EMB_CHECK_EQ_INT(r.hard_x, -1);
}

EMB_TEST(gold_tolerance_of_three_passes)
{
    uint8_t a[10 * 10 * 4];
    uint8_t b[10 * 10 * 4];
    fill_px(b, 10 * 10, 100, 100, 100, 255);
    fill_px(a, 10 * 10, 103, 97, 100, 252);
    emb_gold_report r = emb_gold_compare(a, b, 10, 10);
    EMB_CHECK(emb_gold_report_pass(&r));
    EMB_CHECK_EQ_INT(r.mismatched, 0);
    EMB_CHECK_EQ_INT(r.max_diff, 3);
}

EMB_TEST(gold_one_channel_past_tolerance_is_a_mismatch)
{
    /* 1000 pixels: 5 mismatches is exactly 0.5% and passes, 6 fails. */
    uint8_t a[1000 * 4];
    uint8_t b[1000 * 4];
    fill_px(a, 1000, 50, 50, 50, 255);
    fill_px(b, 1000, 50, 50, 50, 255);

    for (int i = 0; i < 5; i++) {
        a[i * 4 + 1] = 54; /* off by 4 in G */
    }
    emb_gold_report r = emb_gold_compare(a, b, 100, 10);
    EMB_CHECK_EQ_INT(r.mismatched, 5);
    EMB_CHECK_EQ_INT(r.max_diff, 4);
    EMB_CHECK(emb_gold_report_pass(&r));

    a[5 * 4 + 3] = 240; /* sixth mismatch: alpha off by 15 */
    r = emb_gold_compare(a, b, 100, 10);
    EMB_CHECK_EQ_INT(r.mismatched, 6);
    EMB_CHECK(!emb_gold_report_pass(&r));
}

EMB_TEST(gold_hard_limit_fails_even_when_rare)
{
    /* One pixel off by 65 out of 1000 is 0.1%, under the 0.5% budget, but still a bug. */
    uint8_t a[1000 * 4];
    uint8_t b[1000 * 4];
    fill_px(a, 1000, 0, 0, 0, 255);
    fill_px(b, 1000, 0, 0, 0, 255);
    a[(7 * 100 + 42) * 4 + 2] = 65;
    emb_gold_report r = emb_gold_compare(a, b, 100, 10);
    EMB_CHECK(!emb_gold_report_pass(&r));
    EMB_CHECK_EQ_INT(r.hard_x, 42);
    EMB_CHECK_EQ_INT(r.hard_y, 7);
    EMB_CHECK_EQ_INT(r.max_diff, 65);
    EMB_CHECK_EQ_INT(r.mismatched, 1);
}

EMB_TEST(gold_exactly_at_hard_limit_is_not_a_bug)
{
    uint8_t a[1000 * 4];
    uint8_t b[1000 * 4];
    fill_px(a, 1000, 0, 0, 0, 255);
    fill_px(b, 1000, 0, 0, 0, 255);
    a[500 * 4] = 64;
    emb_gold_report r = emb_gold_compare(a, b, 100, 10);
    EMB_CHECK(emb_gold_report_pass(&r));
    EMB_CHECK_EQ_INT(r.hard_x, -1);
    EMB_CHECK_EQ_INT(r.max_diff, 64);
}

EMB_TEST(gold_threshold_boundary_on_small_image)
{
    /* 200 pixels: 1 mismatch is 0.5% (pass), 2 mismatches is 1% (fail). */
    uint8_t a[200 * 4];
    uint8_t b[200 * 4];
    fill_px(a, 200, 9, 9, 9, 255);
    fill_px(b, 200, 9, 9, 9, 255);
    a[0] = 20;
    emb_gold_report r = emb_gold_compare(a, b, 20, 10);
    EMB_CHECK(emb_gold_report_pass(&r));
    a[4] = 20;
    r = emb_gold_compare(a, b, 20, 10);
    EMB_CHECK(!emb_gold_report_pass(&r));
}

EMB_TEST(gold_diff_image_colors)
{
    uint8_t a[4 * 4 * 4];
    uint8_t b[4 * 4 * 4];
    uint8_t d[4 * 4 * 4];
    fill_px(a, 16, 200, 100, 40, 255);
    fill_px(b, 16, 200, 100, 40, 255);
    a[1 * 4] = 210; /* pixel 1: soft mismatch, diff 10 */
    a[2 * 4] = 10;  /* pixel 2: hard mismatch, diff 190 */
    emb_gold_make_diff(a, b, 4, 4, d);

    EMB_CHECK_EQ_INT(d[0], 50);  /* matching: actual / 4 */
    EMB_CHECK_EQ_INT(d[1], 25);
    EMB_CHECK_EQ_INT(d[2], 10);
    EMB_CHECK_EQ_INT(d[3], 255);
    EMB_CHECK_PIXEL(d, 4, 1, 0, 255, 0, 255, 255); /* soft: magenta */
    EMB_CHECK_PIXEL(d, 4, 2, 0, 255, 0, 0, 255);   /* hard: red */
    EMB_CHECK_PIXEL(d, 4, 3, 3, 50, 25, 10, 255);  /* matching again */
}

EMB_TEST(gold_pixel_check_passes_on_exact_match)
{
    uint8_t px[2 * 2 * 4] = {
        1, 2, 3, 4,   5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
    };
    EMB_CHECK_PIXEL(px, 2, 0, 0, 1, 2, 3, 4);
    EMB_CHECK_PIXEL(px, 2, 1, 0, 5, 6, 7, 8);
    EMB_CHECK_PIXEL(px, 2, 0, 1, 9, 10, 11, 12);
    EMB_CHECK_PIXEL(px, 2, 1, 1, 13, 14, 15, 16);
}

EMB_TEST_MAIN()
