#pragma once
/*
 * Internal base library header. Include this one header from the base sources and from the base
 * unit tests. Cross-module symbols use the embi_ prefix; everything else is static.
 */
#include <ember/ember.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* C23 unreachable() comes from stddef.h. Toolchains without it (MinGW GCC 16.1 at
 * C:/ProgramData/mingw64) get the compiler builtin instead. */
#ifndef unreachable
#if defined(__GNUC__) || defined(__clang__)
#define unreachable() __builtin_unreachable()
#elif defined(_MSC_VER)
#define unreachable() __assume(0)
#endif
#endif

#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_stdinc.h>

/* ------------------------------------------------------------------ logging and assertions */

void embi_log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void embi_log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void embi_log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void embi_log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifndef NDEBUG
#define EMBI_ASSERT(x)                                                                         \
    do {                                                                                       \
        if (!(x)) {                                                                            \
            embi_log_error("assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__);            \
            SDL_TriggerBreakpoint();                                                           \
            abort();                                                                           \
        }                                                                                      \
    } while (0)
#else
#define EMBI_ASSERT(x) ((void)sizeof((x) ? 1 : 0))
#endif

#define EMBI_UNREACHABLE()                                                                     \
    do {                                                                                       \
        EMBI_ASSERT(0);                                                                        \
        unreachable();                                                                         \
    } while (0)

/* ------------------------------------------------------------------ allocator */

/* Allocation failure is fatal: it logs and aborts. Zero-size requests return a unique pointer.
 * Frees must pass the size that was requested. Live and peak byte counts are atomic. */
[[nodiscard]] void *embi_alloc(size_t size);
[[nodiscard]] void *embi_calloc(size_t count, size_t size);
[[nodiscard]] void *embi_realloc(void *p, size_t old_size, size_t new_size);
void embi_free(void *p, size_t size);
size_t embi_mem_live(void);
size_t embi_mem_peak(void);
[[noreturn]] void embi_oom(size_t bytes);

/* ------------------------------------------------------------------ arena */

typedef struct embi_arena_block {
    struct embi_arena_block *next;
    size_t cap;  /* usable bytes in data[] */
    size_t used; /* bytes handed out from data[] */
    unsigned char data[];
} embi_arena_block;

/* Blocks are chained in creation order. Only the tail (last) is ever bumped, so a mark taken
 * at the tail can be rolled back by freeing everything created after it. */
typedef struct embi_arena {
    embi_arena_block *first;
    embi_arena_block *last;
    size_t block_size;
    struct embi_arena_pf *pf; /* FP2a: blocks being made ahead on a thread (embi_arena_prefault_start), or nullptr */
} embi_arena;

typedef struct embi_arena_pos {
    embi_arena_block *block; /* nullptr when the arena had no block at mark time */
    size_t used;
} embi_arena_pos;

void embi_arena_init(embi_arena *a, size_t block_size);
[[nodiscard]] void *embi_arena_alloc(embi_arena *a, size_t size, size_t align);
embi_arena_pos embi_arena_mark(const embi_arena *a);
void embi_arena_reset_to(embi_arena *a, embi_arena_pos pos);
void embi_arena_reset(embi_arena *a);
void embi_arena_free(embi_arena *a);
/* FP2a: a thread makes blocks of the arena's block size ahead of the owner and touches their pages, up to about `bytes` in
 * all, so that the owner's first touch of fresh memory (a page fault per 4 KB, 1 ms per 2 MB on Windows) happens on
 * another core while it works. embi_arena_alloc takes the ready blocks before allocating its own; embi_arena_free (or
 * embi_arena_prefault_stop) stops the thread and frees the blocks nobody took. Call it before the first allocation. A
 * failure to start the thread is silent (the arena then allocates as before). */
void embi_arena_prefault_start(embi_arena *a, size_t bytes);
void embi_arena_prefault_stop(embi_arena *a);
/* WPTASAN, a test hook: the poison mode of every arena of the process. While it is on, the memory an arena gives back
 * (embi_arena_reset, embi_arena_reset_to, embi_arena_free) is filled with EMBI_ARENA_POISON bytes and its blocks are kept
 * aside instead of being freed or bumped into again (embi_arena_reset starts a new block), so a pointer that outlived its
 * arena reads the poison deterministically, without ASan. Turning it off frees what was kept. The tail of the block that a
 * partial embi_arena_reset_to keeps is poisoned but reused. Meant for single-threaded tests; while it is off a reset costs
 * one relaxed atomic load more. */
#define EMBI_ARENA_POISON 0xA5u
void embi_arena_debug_poison(bool on);
bool embi_arena_debug_poison_on(void);
/* True when the n bytes at p (n > 0) are all EMBI_ARENA_POISON. Meaningful for arena memory while poison is on. */
bool embi_arena_debug_poisoned(const void *p, size_t n);
/* The checks of the poison mode add the poisoned values they found here (the style engine and the animation layer, for the
 * styles their restyles read), so that a runner can tell per test without reaching into a document. */
void embi_arena_debug_poison_note(uint32_t n);
/* Frees the arena at once, also in poison mode (the scratch memory of the poison checks themselves). */
void embi_arena_debug_free_now(embi_arena *a);
uint64_t embi_arena_debug_poison_noted(void);

/* ------------------------------------------------------------------ hash map */

/* dist is probe distance + 1; 0 marks an empty slot. */
typedef struct embi_hash_slot {
    uint64_t key;
    uint64_t val;
    uint32_t dist;
} embi_hash_slot;

/* Robin Hood open addressing, u64 key to u64 value, backward-shift deletion, load factor 0.85.
 * A zeroed embi_hashmap is an empty map. Put, remove and clear invalidate iterators. */
typedef struct embi_hashmap {
    embi_hash_slot *slots;
    uint32_t cap; /* power of two, or 0 when unallocated */
    uint32_t len;
} embi_hashmap;

typedef struct embi_hashmap_iter {
    const embi_hashmap *map;
    uint32_t pos;
} embi_hashmap_iter;

uint64_t embi_hash64(uint64_t x);
uint64_t embi_hash_bytes(const void *p, size_t n, uint64_t seed);
/* As embi_hash_bytes for short inputs; past 2 KB only the length, the first and last 512 bytes and 64 words spread over
 * the rest are read. For keys whose hits compare the whole text anyway (a URL cache: a data: URL is 100 KB). */
uint64_t embi_hash_sampled(const void *p, size_t n, uint64_t seed);

void embi_hashmap_init(embi_hashmap *m);
void embi_hashmap_free(embi_hashmap *m);
bool embi_hashmap_get(const embi_hashmap *m, uint64_t key, uint64_t *out);
/* Returns true when the key was newly inserted, false when an existing value was replaced. */
bool embi_hashmap_put(embi_hashmap *m, uint64_t key, uint64_t val);
/* Returns true when the key existed and was removed. */
bool embi_hashmap_remove(embi_hashmap *m, uint64_t key);
/* Remove and return the value in one probe. */
bool embi_hashmap_take(embi_hashmap *m, uint64_t key, uint64_t *out);
/* Remove the entry only when its value is val (one probe). */
bool embi_hashmap_remove_if(embi_hashmap *m, uint64_t key, uint64_t val);
/* Insert a key the caller knows is absent (no probe for it first). */
void embi_hashmap_insert_new(embi_hashmap *m, uint64_t key, uint64_t val);
/* The value slot of key, inserting init first when the key is absent (*added tells). The pointer is good until the next put,
 * insert, remove or clear. */
uint64_t *embi_hashmap_ref(embi_hashmap *m, uint64_t key, uint64_t init, bool *added);
void embi_hashmap_clear(embi_hashmap *m);
uint32_t embi_hashmap_len(const embi_hashmap *m);
embi_hashmap_iter embi_hashmap_iter_begin(const embi_hashmap *m);
bool embi_hashmap_iter_next(embi_hashmap_iter *it, uint64_t *key, uint64_t *val);

/* ------------------------------------------------------------------ strings */

/* Non-owning UTF-8 view. */
typedef struct embi_str {
    const char *ptr;
    uint32_t len;
} embi_str;

#define EMBI_STR(lit) ((embi_str){(lit), sizeof(lit) - 1})

/* Decodes one code point at *pp, advances *pp, and never reads at or past end. Invalid input
 * yields U+FFFD and consumes the maximal invalid subpart per WHATWG (one byte for a bad lead). */
uint32_t embi_utf8_decode(const char **pp, const char *end);
/* Writes 1 to 4 bytes to out (at least 4 bytes). Invalid code points encode as U+FFFD. */
size_t embi_utf8_encode(uint32_t cp, char *out);
/* Returns the number of UTF-16 units needed. Writes at most cap units to out (out may be
 * nullptr when cap is 0). */
size_t embi_utf8_to_utf16(embi_str s, uint16_t *out, size_t cap);
/* Returns the number of UTF-8 bytes needed. Writes at most cap bytes; lone surrogates become
 * U+FFFD. */
size_t embi_utf16_to_utf8(const uint16_t *in, size_t count, char *out, size_t cap);

bool embi_str_eq(embi_str a, embi_str b);
/* ASCII case-insensitive. */
bool embi_str_ieq(embi_str a, embi_str b);
/* ASCII case-insensitive ordering: negative, zero or positive. */
int embi_str_icmp(embi_str a, embi_str b);

/* String builder. Zeroed is empty. */
typedef struct embi_sb {
    char *data;
    uint32_t len;
    uint32_t cap;
} embi_sb;

void embi_sb_free(embi_sb *sb);
void embi_sb_clear(embi_sb *sb);
void embi_sb_append(embi_sb *sb, embi_str s);
void embi_sb_append_char(embi_sb *sb, char c);
void embi_sb_append_cp(embi_sb *sb, uint32_t cp);
void embi_sb_append_u64(embi_sb *sb, uint64_t v);
void embi_sb_append_i64(embi_sb *sb, int64_t v);
/* Uses "%.17g" for now; shortest round-trip formatting is not required yet. */
void embi_sb_append_f64(embi_sb *sb, double v);
/* NUL-terminates the buffer without counting the terminator in len. Never returns nullptr. */
const char *embi_sb_cstr(embi_sb *sb);

/* ------------------------------------------------------------------ LZ (lz.c) */

/* Expands a stream made by tools/lz (EMBER_MIN_SIZE builds embed some assets this way) into dst, which must hold exactly
 * cap bytes. False when the stream is damaged or does not yield exactly cap bytes. */
bool embi_lz_decode(const uint8_t *src, size_t n, uint8_t *dst, size_t cap);

/* The assets are stored as tools/lz writes them: a 4-byte little-endian raw size, then the stream. embi_lz_raw_size reads
 * the size (0 when n < 4). embi_lz_unpack returns an embi_alloc'd expansion with a NUL after the last byte (free it with
 * embi_free(p, raw + 1)), or nullptr on a damaged stream; *raw_size, if given, receives the size. */
size_t embi_lz_raw_size(const uint8_t *packed, size_t n);
uint8_t *embi_lz_unpack(const uint8_t *packed, size_t n, size_t *raw_size);

/* ------------------------------------------------------------------ base64 (base64.c) */

/* Forgiving-base64 decode (WHATWG Infra): ASCII whitespace is removed, then padding rules apply.
 * Decoded bytes are appended to out (a byte string in an embi_sb). Returns false on failure, in
 * which case out is unchanged. */
bool embi_base64_decode(embi_str in, embi_sb *out);
/* Standard alphabet with '=' padding. Appends to out. */
void embi_base64_encode(const unsigned char *p, size_t n, embi_sb *out);

/* ------------------------------------------------------------------ atoms */

/* Interned string id. 0 is reserved and never returned by intern. */
typedef uint32_t embi_atom;

embi_atom embi_atom_intern(const char *p, uint32_t len);
/* Looks the string up without inserting it. Returns 0 (never a valid atom) when it was never
 * interned, so lookup-only callers do not grow the table with arbitrary script strings. */
embi_atom embi_atom_find(const char *p, uint32_t len);
/* Returns the empty string for id 0 or an unknown id. The bytes stay valid until
 * embi_atom_shutdown. */
embi_str embi_atom_str(embi_atom id);
/* The table starts with the DOM's static atoms (src/base/static_atoms.inc, ids 1..N), so those
 * ids are fixed whichever module interns first. */
/* Frees the atom table. Call only when no other thread uses atoms. A later intern rebuilds it. */
void embi_atom_shutdown(void);

#include "base/vec.h"
