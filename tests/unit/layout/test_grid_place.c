/*
 * Grid item placement (W3f, CSS Grid 2 section 8.5): definite and auto placement on a fixed explicit
 * grid. Values are derived by hand from the spec; the index of a line L is L - 1 + pre.
 */
#include "emb_test.h"
#include "layout/grid_int.h"

static gpl line_at(int32_t n)
{
    gpl g;
    memset(&g, 0, sizeof g);
    g.set = true;
    g.num = n;
    return g;
}

static gpl auto_pl(void)
{
    gpl g;
    memset(&g, 0, sizeof g);
    return g;
}

/* A 2-row, 3-column explicit grid with no named lines or areas, row flow, sparse. */
static gplace_ctx ctx_2x3(void)
{
    gplace_ctx p;
    memset(&p, 0, sizeof p);
    p.nex[0] = 2;
    p.nex[1] = 3;
    p.row_flow = true;
    return p;
}

EMB_TEST(grid_place_auto_items_fill_rows_in_order)
{
    /* Three fully auto items go to row 0 columns 0, 1, 2 (row flow, section 8.5 step 4). */
    gitem it[3];
    memset(it, 0, sizeof it);
    for (int i = 0; i < 3; i++) {
        for (int ax = 0; ax < 2; ax++) {
            it[i].pl[ax][0] = auto_pl();
            it[i].pl[ax][1] = auto_pl();
        }
    }
    gplace_ctx p = ctx_2x3();
    uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
    EMB_CHECK(embi_grid_place(&p, it, 3, pre, total));
    EMB_CHECK_EQ_INT(it[0].st[1], 0);
    EMB_CHECK_EQ_INT(it[1].st[1], 1);
    EMB_CHECK_EQ_INT(it[2].st[1], 2);
    EMB_CHECK_EQ_INT(it[2].st[0], 0);
    EMB_CHECK_EQ_INT(it[0].en[1], 1);
    EMB_CHECK_EQ_INT(total[1], 3);
}

EMB_TEST(grid_place_definite_column_keeps_its_column)
{
    /* Item 1 is at column line 2 (index 1) and rows auto: it goes to row 0 when that cell is free. */
    gitem it[2];
    memset(it, 0, sizeof it);
    for (int i = 0; i < 2; i++) {
        for (int ax = 0; ax < 2; ax++) {
            it[i].pl[ax][0] = auto_pl();
            it[i].pl[ax][1] = auto_pl();
        }
    }
    it[0].pl[1][0] = line_at(2);
    it[0].pl[1][1] = line_at(3);
    gplace_ctx p = ctx_2x3();
    uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
    EMB_CHECK(embi_grid_place(&p, it, 2, pre, total));
    EMB_CHECK_EQ_INT(it[0].st[1], 1);
    EMB_CHECK_EQ_INT(it[0].en[1], 2);
    EMB_CHECK_EQ_INT(it[0].st[0], 0);
}

EMB_TEST(grid_place_span_item_covers_its_tracks)
{
    /* grid-column: span 2 on an auto item covers two columns, the first free one (index 0). */
    gitem it[1];
    memset(it, 0, sizeof it);
    it[0].pl[0][0] = auto_pl();
    it[0].pl[0][1] = auto_pl();
    it[0].pl[1][0] = auto_pl();
    it[0].pl[1][1].set = true;
    it[0].pl[1][1].span = true;
    it[0].pl[1][1].num = 2;
    gplace_ctx p = ctx_2x3();
    uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
    EMB_CHECK(embi_grid_place(&p, it, 1, pre, total));
    EMB_CHECK_EQ_INT(it[0].st[1], 0);
    EMB_CHECK_EQ_INT(it[0].en[1], 2);
}

EMB_TEST(grid_place_interleaved_auto_and_column_locked_share_one_cursor)
{
    /* Items in order: A auto, B locked to column 1, C auto. Sparse: A takes (0,0). B's column is behind the
     * cursor (column 1 of row 0), so B moves to row 1; C searches from the cursor at row 1 column 0, skips
     * B, and takes (1,1). Dense: B starts at the first row, so C goes back to row 0 column 1. */
    for (int dense = 0; dense < 2; dense++) {
        gitem it[3];
        memset(it, 0, sizeof it);
        for (int i = 0; i < 3; i++) {
            it[i].pl[0][0] = auto_pl();
            it[i].pl[0][1] = auto_pl();
            it[i].pl[1][0] = auto_pl();
            it[i].pl[1][1] = auto_pl();
        }
        it[1].pl[1][0] = line_at(1);
        gplace_ctx p = ctx_2x3();
        p.dense = dense != 0;
        uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
        EMB_CHECK(embi_grid_place(&p, it, 3, pre, total));
        EMB_CHECK_EQ_INT(it[0].st[0], 0);
        EMB_CHECK_EQ_INT(it[0].st[1], 0);
        EMB_CHECK_EQ_INT(it[1].st[0], 1);
        EMB_CHECK_EQ_INT(it[1].st[1], 0);
        EMB_CHECK_EQ_INT(it[2].st[0], dense ? 0 : 1);
        EMB_CHECK_EQ_INT(it[2].st[1], 1);
    }
}

EMB_TEST(grid_place_both_spans_keep_the_start_span)
{
    /* grid-column: span 2 / span 3: the end span is removed and the start span (2) stays. */
    gitem it[1];
    memset(it, 0, sizeof it);
    it[0].pl[0][0] = auto_pl();
    it[0].pl[0][1] = auto_pl();
    it[0].pl[1][0].set = true;
    it[0].pl[1][0].span = true;
    it[0].pl[1][0].num = 2;
    it[0].pl[1][1].set = true;
    it[0].pl[1][1].span = true;
    it[0].pl[1][1].num = 3;
    gplace_ctx p = ctx_2x3();
    uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
    EMB_CHECK(embi_grid_place(&p, it, 1, pre, total));
    EMB_CHECK_EQ_INT(it[0].st[1], 0);
    EMB_CHECK_EQ_INT(it[0].en[1], 2);
}

EMB_TEST(grid_place_named_span_counts_named_lines)
{
    /* Lines named foo at line 2 and line 4 of a 3-column grid. grid-column: 1 / span 2 foo covers the second
     * foo line after line 1, which is line 4: columns 0 to 3. */
    static embi_atom foo_name;
    foo_name = embi_atom_intern("foo", 3);
    gname lines[2] = {{1, foo_name}, {3, foo_name}};
    gplace_ctx p = ctx_2x3();
    p.names[1].data = lines;
    p.names[1].len = 2;
    p.names[1].cap = 2;
    gitem it[1];
    memset(it, 0, sizeof it);
    it[0].pl[0][0] = auto_pl();
    it[0].pl[0][1] = auto_pl();
    it[0].pl[1][0] = line_at(1);
    it[0].pl[1][1].set = true;
    it[0].pl[1][1].span = true;
    it[0].pl[1][1].num = 2;
    it[0].pl[1][1].name = foo_name;
    uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
    EMB_CHECK(embi_grid_place(&p, it, 1, pre, total));
    EMB_CHECK_EQ_INT(it[0].st[1], 0);
    EMB_CHECK_EQ_INT(it[0].en[1], 3);
}

EMB_TEST(grid_place_span_wider_than_the_grid_grows_it)
{
    /* An auto item spanning 5 columns of a 3-column grid grows the implicit grid to 5 columns. */
    gitem it[1];
    memset(it, 0, sizeof it);
    it[0].pl[0][0] = auto_pl();
    it[0].pl[0][1] = auto_pl();
    it[0].pl[1][0] = auto_pl();
    it[0].pl[1][1].set = true;
    it[0].pl[1][1].span = true;
    it[0].pl[1][1].num = 5;
    gplace_ctx p = ctx_2x3();
    uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
    EMB_CHECK(embi_grid_place(&p, it, 1, pre, total));
    EMB_CHECK_EQ_INT(it[0].st[1], 0);
    EMB_CHECK_EQ_INT(it[0].en[1], 5);
    EMB_CHECK_EQ_INT(total[1], 5);
}

EMB_TEST(grid_place_huge_grid_places_without_occupancy)
{
    /* 5000 x 5000 explicit cells is over the 16M cell occupancy limit, so the placement runs without the
     * occupancy check: two items at the same definite cell are both placed there, with no failure. */
    gitem it[2];
    memset(it, 0, sizeof it);
    for (int i = 0; i < 2; i++) {
        it[i].pl[0][0] = line_at(1);
        it[i].pl[0][1] = line_at(2);
        it[i].pl[1][0] = line_at(1);
        it[i].pl[1][1] = line_at(2);
    }
    gplace_ctx p;
    memset(&p, 0, sizeof p);
    p.nex[0] = 5000;
    p.nex[1] = 5000;
    p.row_flow = true;
    uint32_t pre[2] = {0, 0}, total[2] = {0, 0};
    EMB_CHECK(embi_grid_place(&p, it, 2, pre, total));
    EMB_CHECK_EQ_INT(it[0].st[0], 0);
    EMB_CHECK_EQ_INT(it[1].st[0], 0);
    EMB_CHECK_EQ_INT(total[0], 5000);
}

EMB_TEST_MAIN()
