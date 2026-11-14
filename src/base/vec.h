#pragma once
/*
 * Growable array macros. Included at the end of base.h; include base.h, not this file.
 *
 *     EMBI_VEC(int) v = {0};
 *     embi_vec_push(&v, 42);
 *     int x = embi_vec_at(&v, 0);
 *     embi_vec_free(&v);
 *
 * Any struct with data, len and cap fields of matching types works (embi_sb does). Arguments
 * are evaluated more than once, so pass side-effect-free lvalues.
 */

/* Growth x1.5 with a minimum capacity of 8, saturating at UINT32_MAX and never below `need`. */
static inline uint32_t embi_vec_grow_to(uint32_t cap, uint32_t need)
{
    uint64_t next = (uint64_t)cap + cap / 2;
    if (next < 8) {
        next = 8;
    }
    if (next > UINT32_MAX) {
        next = UINT32_MAX;
    }
    if (next < need) {
        next = need;
    }
    return (uint32_t)next;
}

/* Decrements *len and returns the new length, which is the index of the popped element. */
static inline uint32_t embi_vec_dec_(uint32_t *len)
{
    EMBI_ASSERT(*len > 0);
    return --*len;
}

static inline uint32_t embi_vec_check_(uint32_t i, uint32_t len)
{
    EMBI_ASSERT(i < len);
    return i;
}

#define EMBI_VEC(T)                                                                            \
    struct {                                                                                   \
        T *data;                                                                               \
        uint32_t len;                                                                          \
        uint32_t cap;                                                                          \
    }

#define embi_vec_reserve(v, n)                                                                 \
    do {                                                                                       \
        uint32_t embi_vr_need_ = (uint32_t)(n);                                                \
        if (embi_vr_need_ > (v)->cap) {                                                        \
            uint32_t embi_vr_cap_ = embi_vec_grow_to((v)->cap, embi_vr_need_);                 \
            (v)->data = embi_realloc((v)->data, (size_t)(v)->cap * sizeof((v)->data[0]),       \
                                     (size_t)embi_vr_cap_ * sizeof((v)->data[0]));             \
            (v)->cap = embi_vr_cap_;                                                           \
        }                                                                                      \
    } while (0)

#define embi_vec_push(v, x)                                                                    \
    do {                                                                                       \
        if ((v)->len == (v)->cap) {                                                            \
            embi_vec_reserve((v), (v)->len + 1u);                                              \
        }                                                                                      \
        (v)->data[(v)->len] = (x);                                                             \
        (v)->len++;                                                                            \
    } while (0)

/* Yields an lvalue for the popped element. It stays valid until the next push. */
#define embi_vec_pop(v) ((v)->data[embi_vec_dec_(&(v)->len)])

/* Bounds-checked in debug builds. */
#define embi_vec_at(v, i) ((v)->data[embi_vec_check_((uint32_t)(i), (v)->len)])

#define embi_vec_clear(v) ((void)((v)->len = 0))

#define embi_vec_free(v)                                                                       \
    do {                                                                                       \
        embi_free((v)->data, (size_t)(v)->cap * sizeof((v)->data[0]));                         \
        (v)->data = nullptr;                                                                   \
        (v)->len = 0;                                                                          \
        (v)->cap = 0;                                                                          \
    } while (0)
