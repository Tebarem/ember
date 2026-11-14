#include "base/base.h"

#include <stdatomic.h>

static atomic_size_t g_live;
static atomic_size_t g_peak;

static void account_add(size_t n)
{
    size_t live = atomic_fetch_add_explicit(&g_live, n, memory_order_relaxed) + n;
    size_t peak = atomic_load_explicit(&g_peak, memory_order_relaxed);
    while (live > peak && !atomic_compare_exchange_weak_explicit(&g_peak, &peak, live,
                                                                 memory_order_relaxed,
                                                                 memory_order_relaxed)) {
    }
}

static void account_sub(size_t n)
{
    size_t prev = atomic_fetch_sub_explicit(&g_live, n, memory_order_relaxed);
    EMBI_ASSERT(prev >= n);
    (void)prev;
}

void embi_oom(size_t bytes)
{
    embi_log_error("out of memory (requested %llu bytes)", (unsigned long long)bytes);
    abort();
}

void *embi_alloc(size_t size)
{
    void *p = SDL_malloc(size ? size : 1);
    if (!p) {
        embi_oom(size);
    }
    account_add(size);
    return p;
}

void *embi_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size) {
        embi_oom(SIZE_MAX);
    }
    size_t total = count * size;
    void *p = SDL_calloc(1, total ? total : 1);
    if (!p) {
        embi_oom(total);
    }
    account_add(total);
    return p;
}

void *embi_realloc(void *p, size_t old_size, size_t new_size)
{
    if (!p) {
        EMBI_ASSERT(old_size == 0);
        return embi_alloc(new_size);
    }
    void *q = SDL_realloc(p, new_size ? new_size : 1);
    if (!q) {
        embi_oom(new_size);
    }
    if (new_size >= old_size) {
        account_add(new_size - old_size);
    } else {
        account_sub(old_size - new_size);
    }
    return q;
}

void embi_free(void *p, size_t size)
{
    if (!p) {
        return;
    }
    SDL_free(p);
    account_sub(size);
}

size_t embi_mem_live(void)
{
    return atomic_load_explicit(&g_live, memory_order_relaxed);
}

size_t embi_mem_peak(void)
{
    return atomic_load_explicit(&g_peak, memory_order_relaxed);
}
