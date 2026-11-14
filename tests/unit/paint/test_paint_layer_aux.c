/*
 * The marker pairing of opacity groups (W4c, finish_layer in paint.c, read by damage.c and layer.c): every LAYER_BEGIN
 * left in a display list has an aux that indexes its LAYER_END, and that END's aux is the begin, for nested groups,
 * for a group that folds into its one item, and for a group with nothing in it, which is dropped.
 */
#include "paint_harness.h"

#include <string.h>

/* Checks every LAYER_BEGIN of d: its aux is a later index that holds a LAYER_END whose aux is the begin. *groups receives
 * the number of groups seen. */
static void check_layer_pairs(const embi_dl *d, int *groups)
{
    *groups = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind != EMBI_DI_LAYER_BEGIN) {
            continue;
        }
        uint32_t j = d->items.data[i].aux;
        EMB_CHECK(j > i);
        EMB_REQUIRE(j < d->items.len);
        EMB_CHECK(d->items.data[j].kind == EMBI_DI_LAYER_END);
        EMB_CHECK(d->items.data[j].aux == i);
        (*groups)++;
    }
}

EMB_TEST(nested_groups_pair_their_markers)
{
    /* Two overlapping children in a group (kept as a group), and a group nested in a group. */
    ph h = ph_make("<div style=\"opacity:.5\">"
                   "<div style=\"opacity:.5;background:#f00;height:10px\"></div>"
                   "<div style=\"opacity:.5;background:#00f;height:10px\"></div>"
                   "<div style=\"opacity:.6;background:#0f0;height:30px\">"
                   "<div style=\"opacity:.4;background:#ff0;height:10px\"></div>"
                   "<div style=\"opacity:.4;background:#0ff;height:10px\"></div>"
                   "</div></div>",
                   "");
    int groups = 0;
    check_layer_pairs(&h.dl, &groups);
    EMB_CHECK(groups >= 2);
    ph_free(&h);
}

EMB_TEST(folded_and_dropped_groups_leave_no_open_markers)
{
    /* One item under an opacity: the group folds into the item, so no LAYER_BEGIN is left. */
    ph folded = ph_make("<div style=\"opacity:.5;background:#f00;height:10px\"></div>", "");
    int groups = -1;
    check_layer_pairs(&folded.dl, &groups);
    EMB_CHECK(groups == 0);
    EMB_CHECK(ph_find(&folded.dl, 0, EMBI_DI_LAYER_BEGIN) < 0);
    ph_free(&folded);

    /* An opacity group with nothing to paint is dropped with its markers. */
    ph empty = ph_make("<div style=\"opacity:.5;height:10px\"></div>", "");
    check_layer_pairs(&empty.dl, &groups);
    EMB_CHECK(groups == 0);
    EMB_CHECK(ph_find(&empty.dl, 0, EMBI_DI_LAYER_BEGIN) < 0);
    ph_free(&empty);

    /* A folded group next to a kept one: the kept group's markers still pair. */
    ph mixed = ph_make("<div style=\"opacity:.5;background:#f00;height:10px\"></div>"
                       "<div style=\"opacity:.5\">"
                       "<div style=\"background:#00f;height:10px\"></div>"
                       "<div style=\"background:#0f0;height:10px\"></div></div>",
                       "");
    check_layer_pairs(&mixed.dl, &groups);
    EMB_CHECK(groups == 1);
    ph_free(&mixed);
}

EMB_TEST_MAIN()
