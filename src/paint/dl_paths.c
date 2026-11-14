/*
 * The paths of a display list (R4c): EMBI_DI_PATH items name paths the list holds a reference to. The paths are reference
 * counted so that the incremental paint cache can copy a list that has some (the copy shares the paths). This file is apart
 * from display.c because it includes the GPU headers, whose type names clash with the CSS ones that display.c sees through
 * paint_int.h.
 */
#include "paint/dl.h"

#include "base/base.h"
#include "gpu/path.h"

#include <stdatomic.h>

struct embi_pathref {
    _Atomic uint32_t refs;
};

/* Adds path (taken: the list gets the first reference) to the table and returns its index. */
uint32_t embi_dl_add_path(embi_dl *d, struct embi_path *path, uint64_t hash);
uint32_t embi_dl_add_path(embi_dl *d, struct embi_path *path, uint64_t hash)
{
    embi_pathref *r = embi_alloc(sizeof *r);
    atomic_init(&r->refs, 1);
    embi_dpath dp = {path, hash, r};
    embi_vec_push(&d->paths, dp);
    return d->paths.len - 1;
}

static void release(embi_dpath *p)
{
    if (p->ref != nullptr && atomic_fetch_sub_explicit(&p->ref->refs, 1, memory_order_acq_rel) == 1) {
        embi_path_destroy(p->path);
        embi_free(p->ref, sizeof *p->ref);
    }
}

/* Drops the list's references and empties the table (the array stays). */
void embi_dl_free_paths(embi_dl *d);
void embi_dl_free_paths(embi_dl *d)
{
    for (uint32_t i = 0; i < d->paths.len; i++) {
        release(&d->paths.data[i]);
    }
    embi_vec_clear(&d->paths);
}

/* dst (already reset) gets a reference to every path of src. */
void embi_dl_copy_paths(embi_dl *dst, const embi_dl *src);
void embi_dl_copy_paths(embi_dl *dst, const embi_dl *src)
{
    for (uint32_t i = 0; i < src->paths.len; i++) {
        embi_dpath p = src->paths.data[i];
        if (p.ref != nullptr) {
            atomic_fetch_add_explicit(&p.ref->refs, 1, memory_order_relaxed);
        }
        embi_vec_push(&dst->paths, p);
    }
}

uint32_t embi_dl_share_path(embi_dl *d, uint32_t pi)
{
    embi_dpath p = d->paths.data[pi];
    if (p.ref != nullptr) {
        atomic_fetch_add_explicit(&p.ref->refs, 1, memory_order_relaxed);
    }
    embi_vec_push(&d->paths, p);
    return d->paths.len - 1;
}
