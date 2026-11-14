/*
 * Hover chains, enter and leave sequences, and click counts (W5d). The pointer's target is the
 * innermost element under it; the enter and leave events of the DOM run over the ancestor chains of the old and the new
 * target.
 */
#include "platform/platform.h"

#include "base/base.h"

uint32_t embi_pf_chain(const embi_node *el, embi_element **out, uint32_t cap)
{
    /* The depth first: a chain longer than cap drops its innermost elements, so every chain of one tree ends at the
     * root and the common-suffix walk of embi_pf_crossings stays correct. */
    uint32_t depth = 0;
    for (const embi_node *k = el; k != nullptr; k = k->parent) {
        if (embi_is_element(k)) {
            depth++;
        }
    }
    uint32_t skip = depth > cap ? depth - cap : 0;
    uint32_t n = 0;
    for (const embi_node *k = el; k != nullptr; k = k->parent) {
        if (!embi_is_element(k)) {
            continue;
        }
        if (skip > 0) {
            skip--;
            continue;
        }
        out[n++] = (embi_element *)(uintptr_t)k;
    }
    return n;
}

embi_pf_crossing embi_pf_crossings(const embi_element *const *old_chain, uint32_t n_old,
                                   const embi_element *const *new_chain, uint32_t n_new,
                                   embi_element **leave, embi_element **enter, uint32_t cap)
{
    embi_pf_crossing r = {0, 0};
    /* The chains end at the root, so the elements both contain are a common suffix: the walk from the root end finds
     * where they part, in linear time. */
    uint32_t i = n_old, j = n_new;
    while (i > 0 && j > 0 && old_chain[i - 1] == new_chain[j - 1]) {
        i--;
        j--;
    }
    /* leave: the old chain from the innermost outwards, up to the common part. */
    for (uint32_t k = 0; k < i && r.nleave < cap; k++) {
        leave[r.nleave++] = (embi_element *)(uintptr_t)old_chain[k];
    }
    /* enter: the new chain from the outermost inwards, down to the common part. */
    for (uint32_t k = j; k > 0 && r.nenter < cap; k--) {
        enter[r.nenter++] = (embi_element *)(uintptr_t)new_chain[k - 1];
    }
    return r;
}

uint32_t embi_pf_click_press(embi_pf_clicks *s, double now_ms, float x, float y, uint32_t button, const void *target)
{
    float dx = x - s->x;
    float dy = y - s->y;
    bool near = dx * dx + dy * dy <= EMBI_PF_CLICK_PX * EMBI_PF_CLICK_PX;
    bool same = s->count > 0 && s->button == button && s->target == target && near &&
                now_ms - s->last_ms <= EMBI_PF_CLICK_MS;
    s->count = same ? s->count + 1 : 1;
    s->button = button;
    s->last_ms = now_ms;
    s->x = x;
    s->y = y;
    s->target = target;
    return s->count;
}
