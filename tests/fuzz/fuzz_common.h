#pragma once
/*
 * Shared helpers for the libFuzzer harnesses (tests/fuzz/fuzz_*.c).
 *
 * Each harness is one LLVMFuzzerTestOneInput. It brackets its work with FUZZ_GUARD_BEGIN() and
 * FUZZ_GUARD_END(): after every input, every byte the input allocated through the base allocator
 * must be back (embi_mem_live), or the harness aborts with a message. That is a per-input leak
 * check (LeakSanitizer only reports at process exit). The global atom table grows with the strings
 * the input interns, so it is dropped before the baseline is taken and again before the final
 * comparison.
 *
 * A harness that keeps state across inputs must not hold atoms in it.
 */
#include "base/base.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The libFuzzer entry point. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

/* Harnesses that use modules with per-thread atom caches define this before including the header,
 * for example #define FUZZ_RESET_HOOKS() embi_html_name_cache_reset(); embi_sel_lower_cache_reset(). */
#ifndef FUZZ_RESET_HOOKS
#define FUZZ_RESET_HOOKS()
#endif

static inline void fuzz_atoms_reset(void)
{
    embi_atom_shutdown();
    FUZZ_RESET_HOOKS()
}

static size_t fuzz_live_base;

#define FUZZ_GUARD_BEGIN()                                                                     \
    do {                                                                                       \
        fuzz_atoms_reset();                                                                    \
        fuzz_live_base = embi_mem_live();                                                      \
    } while (0)

#define FUZZ_GUARD_END()                                                                       \
    do {                                                                                       \
        fuzz_atoms_reset();                                                                    \
        size_t fuzz_live_now = embi_mem_live();                                                \
        if (fuzz_live_now != fuzz_live_base) {                                                 \
            fprintf(stderr, "LEAK: %lld bytes still live after the input (%s:%d)\n",            \
                    (long long)(fuzz_live_now - fuzz_live_base), __FILE__, __LINE__);            \
            abort();                                                                           \
        }                                                                                      \
    } while (0)

/* A reader over the input: the leading bytes of an input choose options, the rest is the payload. */
typedef struct fuzz_in {
    const uint8_t *p;
    size_t n;
} fuzz_in;

static inline uint8_t fuzz_u8(fuzz_in *in)
{
    if (in->n == 0) {
        return 0;
    }
    uint8_t b = in->p[0];
    in->p++;
    in->n--;
    return b;
}

static inline uint16_t fuzz_u16(fuzz_in *in)
{
    uint16_t lo = fuzz_u8(in);
    return (uint16_t)(lo | (uint16_t)fuzz_u8(in) << 8);
}

/* The next n bytes (fewer when the input is shorter). */
static inline fuzz_in fuzz_take(fuzz_in *in, size_t n)
{
    if (n > in->n) {
        n = in->n;
    }
    fuzz_in r = {in->p, n};
    in->p += n;
    in->n -= n;
    return r;
}

/* The payload as a string view; never a null pointer, so it is safe to pass on. */
static inline embi_str fuzz_str(const fuzz_in *in)
{
    static const char empty[1] = {0};
    return (embi_str){in->n ? (const char *)in->p : empty, (uint32_t)in->n};
}

/* Copy of the payload with a NUL after it (some APIs want a C string). Free with embi_free(p, n + 1). */
static inline char *fuzz_dup_z(const fuzz_in *in)
{
    char *s = embi_alloc(in->n + 1);
    if (in->n) {
        memcpy(s, in->p, in->n);
    }
    s[in->n] = '\0';
    return s;
}
