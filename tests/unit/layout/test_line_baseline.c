/*
 * LINE fragments carry their baseline (W4f): bl_first and bl_last of a line are the offset from the line's top to its
 * baseline, the same for the text on it. Before W4f they were NaN and paint derived the baseline from the block.
 */
#include "emb_test.h"

#include "layout_harness.h"

#include <math.h>

static void collect_lines(const embi_frag *f, const embi_frag **out, uint32_t *n, uint32_t cap)
{
    for (const embi_frag *k = f->kind == EMBI_FK_TEXT ? nullptr : f->first; k != nullptr; k = k->next) {
        if (k->kind == EMBI_FK_LINE && *n < cap) {
            out[(*n)++] = k;
        }
        collect_lines(k, out, n, cap);
    }
}

EMB_TEST(line_fragments_carry_their_baseline)
{
    lh h = lh_make("<div id=a>one two three four five six</div><div id=b style='font-size:40px'>big</div>",
                   "#a{width:60px;font-size:16px;line-height:30px}");
    const embi_frag *lines[16];
    uint32_t n = 0;
    collect_lines(embi_layout_root(h.eng, h.doc), lines, &n, 16);
    EMB_CHECK(n >= 3);
    for (uint32_t i = 0; i < n; i++) {
        EMB_CHECK(!isnan(embi_frag_bl_first(lines[i])));
        EMB_CHECK(!isnan(embi_frag_bl_last(lines[i])));
        EMB_CHECK(embi_frag_bl_first(lines[i]) > 0.0f && embi_frag_bl_first(lines[i]) <= lines[i]->h);
    }
    /* The lines of one block share a line height and a strut, so their baselines agree. */
    EMB_CHECK(fabsf(embi_frag_bl_first(lines[0]) - embi_frag_bl_first(lines[1])) < 0.01f);
    lh_free(&h);
}

EMB_TEST_MAIN()
