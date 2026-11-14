/*
 * Painting fixes of the BUGBURN1 reftest burn-down, on the display list: the backgrounds of rows, row groups and columns
 * of a separated-borders table show behind the cells only (WPT table-backgrounds-bs-*-001).
 */
#include "paint_harness.h"

#include <stdio.h>

static uint32_t fills_of(const embi_dl *d, uint32_t rgba)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        n += d->items.data[i].kind == EMBI_DI_FILL && d->items.data[i].colors[0] == rgba ? 1u : 0u;
    }
    return n;
}

#define CELLS "<tr><td>a</td><td>b</td><td>c</td></tr>"

EMB_TEST(row_background_is_painted_once_per_cell_when_border_spacing_leaves_gaps)
{
    ph h = ph_make("<table style=\"border-spacing:6px\"><tr style=\"background:#ff0000\"><td>a</td><td>b</td><td>c</td></tr></table>", "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_RED), 3);
    ph_free(&h);
}

EMB_TEST(row_group_background_is_painted_per_cell_in_every_row)
{
    ph h = ph_make("<table style=\"border-spacing:6px\"><tbody style=\"background:#ff0000\">" CELLS CELLS "</tbody></table>", "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_RED), 6);
    ph_free(&h);
}

EMB_TEST(row_background_is_one_fill_when_the_cells_tile_the_row)
{
    ph h = ph_make("<table style=\"border-spacing:0\"><tr style=\"background:#ff0000\"><td>a</td><td>b</td></tr></table>", "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_RED), 1);
    ph_free(&h);
}

EMB_TEST(column_background_is_painted_per_cell_when_border_spacing_leaves_gaps)
{
    ph h = ph_make("<table style=\"border-spacing:6px\"><col><col style=\"background:#ff0000\"><tr><td>a</td><td>b</td></tr>"
                   "<tr><td>a</td><td>b</td></tr></table>",
                   "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_RED), 2);
    ph_free(&h);
}

EMB_TEST(column_background_with_a_rowspan_cell_is_painted_once_for_the_spanning_cell)
{
    ph h = ph_make("<table style=\"border-spacing:6px\"><col style=\"background:#ff0000\"><col><tr><td rowspan=\"2\">a</td><td>b</td></tr>"
                   "<tr><td>c</td></tr></table>",
                   "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_RED), 1);
    ph_free(&h);
}

EMB_TEST(row_background_over_hundreds_of_cells_is_painted_per_cell)
{
    char html[8192];
    size_t n = 0;
    n += (size_t)snprintf(html + n, sizeof html - n, "<table style=\"border-spacing:2px\"><tr style=\"background:#ff0000\">");
    for (int i = 0; i < 300; i++) {
        n += (size_t)snprintf(html + n, sizeof html - n, "<td></td>");
    }
    snprintf(html + n, sizeof html - n, "</tr></table>");
    ph h = ph_make(html, "");
    EMB_CHECK_EQ_INT(fills_of(&h.dl, PH_RED), 300);
    ph_free(&h);
}

EMB_TEST_MAIN()
