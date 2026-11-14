/*
 * Column rules (CSS Multi-column Layout 1 section 4): painted between the columns that have content, centered in the
 * gap, as long as the columns.
 */
#include "paint_harness.h"

EMB_TEST(column_rule_is_painted_in_the_middle_of_the_gap)
{
    ph h = ph_make("<div style='width:200px;column-count:2;column-gap:20px;column-rule:4px solid #f00'>"
                   "<div style='height:20px'></div><div style='height:20px;overflow:hidden'></div></div>",
                   "");
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *it = &h.dl.items.data[i];
    /* two columns of 90px with a 20px gap: the rule is centered at x = 100 */
    EMB_CHECK(it->rect[0] == 98.0f && it->rect[1] == 0.0f && it->rect[2] == 4.0f && it->rect[3] == 20.0f);
    EMB_CHECK(it->colors[3] == PH_RED);
    EMB_CHECK(it->styles[3] == EMBI_DS_SOLID);
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 1);
    ph_free(&h);
}

EMB_TEST(no_rule_when_only_one_column_has_content)
{
    ph h = ph_make("<div style='width:300px;column-count:3;column-rule:4px solid #f00'>"
                   "<div style='height:20px;overflow:hidden'></div></div>",
                   "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 0); /* one column has content: nothing to separate */
    ph_free(&h);
}

EMB_TEST(three_columns_get_two_rules)
{
    ph h = ph_make("<div style='width:300px;column-count:3;column-gap:0;column-rule:2px dashed #00f'>"
                   "<div style='height:20px'></div><div style='height:20px'></div><div style='height:20px;overflow:hidden'></div></div>",
                   "");
    EMB_CHECK_EQ_INT(ph_count(&h.dl, EMBI_DI_BORDER), 2);
    int i = ph_find(&h.dl, 0, EMBI_DI_BORDER);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].styles[3] == EMBI_DS_DASHED);
    EMB_CHECK(h.dl.items.data[i].rect[0] == 99.0f);
    ph_free(&h);
}

EMB_TEST_MAIN()
