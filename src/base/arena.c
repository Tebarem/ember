#include "base/base.h"

#include <stdatomic.h>
#include <string.h>

#include <SDL3/SDL_thread.h>

/* Sanitizer builds: some users of an arena poison parts of its blocks for ASan (layout fragments); the poison mode of the
 * tests writes whole blocks, so it lifts that first. */
#if defined(__SANITIZE_ADDRESS__)
#define ARENA_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ARENA_ASAN 1
#endif
#endif
#ifdef ARENA_ASAN
#include <sanitizer/asan_interface.h>
#endif

#define EMBI_ARENA_DEFAULT_BLOCK (64u * 1024u)
#define EMBI_ARENA_MAX_ALIGN 64u

static size_t block_bytes(const embi_arena_block *b)
{
    return sizeof *b + b->cap;
}

static embi_arena_block *block_new(size_t cap)
{
    embi_arena_block *b = embi_alloc(sizeof *b + cap);
    b->next = nullptr;
    b->cap = cap;
    b->used = 0;
    return b;
}

/* ---- the poison mode (embi_arena_debug_poison): blocks given back are poisoned and kept on g_quar */
static atomic_bool g_poison;
static atomic_flag g_quar_lock = ATOMIC_FLAG_INIT;
static embi_arena_block *g_quar;

static void quar_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_quar_lock, memory_order_acquire)) {
    }
}

static void quar_unlock(void)
{
    atomic_flag_clear_explicit(&g_quar_lock, memory_order_release);
}

/* Poisons the chain from b and puts it on the quarantine list. */
static void quarantine_chain(embi_arena_block *b)
{
    while (b) {
        embi_arena_block *next = b->next;
#ifdef ARENA_ASAN
        ASAN_UNPOISON_MEMORY_REGION(b->data, b->cap);
#endif
        memset(b->data, (int)EMBI_ARENA_POISON, b->cap);
        quar_lock();
        b->next = g_quar;
        g_quar = b;
        quar_unlock();
        b = next;
    }
}

static void free_now(embi_arena_block *b)
{
    while (b) {
        embi_arena_block *next = b->next;
        embi_free(b, block_bytes(b));
        b = next;
    }
}

static void free_chain(embi_arena_block *b)
{
    if (atomic_load_explicit(&g_poison, memory_order_relaxed)) {
        quarantine_chain(b);
        return;
    }
    while (b) {
        embi_arena_block *next = b->next;
        embi_free(b, block_bytes(b));
        b = next;
    }
}

void embi_arena_debug_poison(bool on)
{
    atomic_store_explicit(&g_poison, on, memory_order_relaxed);
    if (on) {
        return;
    }
    quar_lock();
    embi_arena_block *b = g_quar;
    g_quar = nullptr;
    quar_unlock();
    while (b) {
        embi_arena_block *next = b->next;
        embi_free(b, block_bytes(b));
        b = next;
    }
}

bool embi_arena_debug_poison_on(void)
{
    return atomic_load_explicit(&g_poison, memory_order_relaxed);
}

static atomic_uint_least64_t g_poison_noted;

void embi_arena_debug_poison_note(uint32_t n)
{
    atomic_fetch_add_explicit(&g_poison_noted, n, memory_order_relaxed);
}

uint64_t embi_arena_debug_poison_noted(void)
{
    return atomic_load_explicit(&g_poison_noted, memory_order_relaxed);
}

bool embi_arena_debug_poisoned(const void *p, size_t n)
{
    const unsigned char *c = p;
    if (c == nullptr || n == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (c[i] != EMBI_ARENA_POISON) {
            return false;
        }
    }
    return true;
}

/* Bumps inside one block. Returns nullptr when the aligned request does not fit. */
static void *bump(embi_arena_block *b, size_t size, size_t align)
{
    uintptr_t base = (uintptr_t)b->data;
    uintptr_t aligned = (base + b->used + align - 1) & ~(uintptr_t)(align - 1);
    size_t off = (size_t)(aligned - base);
    if (off > b->cap || size > b->cap - off) {
        return nullptr;
    }
    b->used = off + size;
    return b->data + off;
}

static void append_block(embi_arena *a, embi_arena_block *b)
{
    if (a->last) {
        a->last->next = b;
    } else {
        a->first = b;
    }
    a->last = b;
}

/* The blocks made ahead (embi_arena_prefault_start): a single producer (the thread) and a single consumer (the owner). */
typedef struct embi_arena_pf {
    SDL_Thread *th;
    size_t n;           /* blocks planned */
    size_t cap;         /* the capacity of each */
    size_t taken;       /* the consumer's index */
    atomic_size_t ready; /* blocks[0..ready) are made */
    atomic_bool stop;
    embi_arena_block **blocks;
} embi_arena_pf;

static int pf_main(void *arg)
{
    embi_arena_pf *pf = arg;
    for (size_t i = 0; i < pf->n && !atomic_load_explicit(&pf->stop, memory_order_relaxed); i++) {
        embi_arena_block *b = block_new(pf->cap);
        for (size_t o = 0; o < pf->cap; o += 4096u) {
            b->data[o] = 0;
        }
        b->data[pf->cap - 1u] = 0;
        pf->blocks[i] = b;
        atomic_store_explicit(&pf->ready, i + 1u, memory_order_release);
    }
    return 0;
}

static embi_arena_block *pf_take(embi_arena_pf *pf, size_t cap)
{
    if (cap != pf->cap || pf->taken >= atomic_load_explicit(&pf->ready, memory_order_acquire)) {
        return nullptr;
    }
    return pf->blocks[pf->taken++];
}

void embi_arena_prefault_start(embi_arena *a, size_t bytes)
{
    if (a->pf != nullptr || a->first != nullptr) {
        return;
    }
    size_t n = bytes / a->block_size;
    if (n == 0) {
        return;
    }
    if (n > 1024u) {
        n = 1024u;
    }
    embi_arena_pf *pf = embi_calloc(1, sizeof *pf);
    pf->n = n;
    pf->cap = a->block_size;
    pf->blocks = embi_calloc(n, sizeof *pf->blocks);
    pf->th = SDL_CreateThread(pf_main, "ember-prefault", pf);
    if (pf->th == nullptr) {
        embi_free(pf->blocks, n * sizeof *pf->blocks);
        embi_free(pf, sizeof *pf);
        return;
    }
    a->pf = pf;
}

void embi_arena_prefault_stop(embi_arena *a)
{
    embi_arena_pf *pf = a->pf;
    if (pf == nullptr) {
        return;
    }
    a->pf = nullptr;
    atomic_store_explicit(&pf->stop, true, memory_order_relaxed);
    SDL_WaitThread(pf->th, nullptr);
    size_t ready = atomic_load_explicit(&pf->ready, memory_order_acquire);
    for (size_t i = pf->taken; i < ready; i++) {
        embi_free(pf->blocks[i], block_bytes(pf->blocks[i]));
    }
    embi_free(pf->blocks, pf->n * sizeof *pf->blocks);
    embi_free(pf, sizeof *pf);
}

void embi_arena_init(embi_arena *a, size_t block_size)
{
    a->first = nullptr;
    a->last = nullptr;
    a->pf = nullptr;
    a->block_size = block_size ? block_size : EMBI_ARENA_DEFAULT_BLOCK;
}

void *embi_arena_alloc(embi_arena *a, size_t size, size_t align)
{
    if (align == 0) {
        align = 1;
    }
    EMBI_ASSERT(align <= EMBI_ARENA_MAX_ALIGN && (align & (align - 1)) == 0);
    if (size == 0) {
        size = 1; /* zero-size requests still get a unique address */
    }
    if (size > SIZE_MAX - align) {
        embi_oom(size);
    }
    size_t worst = size + align - 1;

    if (a->last) {
        void *p = bump(a->last, size, align);
        if (p) {
            return p;
        }
    }
    /* Oversized requests get a block of their own sized to fit, and that block becomes the
     * tail. Later small requests start a fresh block once it is full. */
    size_t cap = worst > a->block_size ? worst : a->block_size;
    embi_arena_block *nb = a->pf != nullptr ? pf_take(a->pf, cap) : nullptr;
    if (nb == nullptr) {
        nb = block_new(cap);
    }
    append_block(a, nb);
    return bump(nb, size, align);
}

embi_arena_pos embi_arena_mark(const embi_arena *a)
{
    embi_arena_pos pos = {a->last, a->last ? a->last->used : 0};
    return pos;
}

void embi_arena_reset_to(embi_arena *a, embi_arena_pos pos)
{
    if (!pos.block) {
        free_chain(a->first);
        a->first = nullptr;
        a->last = nullptr;
        return;
    }
    free_chain(pos.block->next);
    pos.block->next = nullptr;
    if (atomic_load_explicit(&g_poison, memory_order_relaxed) && pos.used < pos.block->cap) {
#ifdef ARENA_ASAN
        ASAN_UNPOISON_MEMORY_REGION(pos.block->data + pos.used, pos.block->cap - pos.used);
#endif
        memset(pos.block->data + pos.used, (int)EMBI_ARENA_POISON, pos.block->cap - pos.used);
    }
    pos.block->used = pos.used;
    a->last = pos.block;
}

void embi_arena_reset(embi_arena *a)
{
    if (a->first && atomic_load_explicit(&g_poison, memory_order_relaxed)) {
        /* nothing is bumped into again: a stale pointer keeps reading the poison */
        quarantine_chain(a->first);
        a->first = nullptr;
        a->last = nullptr;
        return;
    }
    if (a->first) {
        embi_arena_pos first = {a->first, 0};
        embi_arena_reset_to(a, first);
    }
}

void embi_arena_free(embi_arena *a)
{
    embi_arena_prefault_stop(a);
    free_chain(a->first);
    a->first = nullptr;
    a->last = nullptr;
}

void embi_arena_debug_free_now(embi_arena *a)
{
    embi_arena_prefault_stop(a);
    free_now(a->first);
    a->first = nullptr;
    a->last = nullptr;
}
