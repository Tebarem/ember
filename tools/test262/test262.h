#pragma once
/*
 * Shared types of the test262 runner (tools/test262).
 * frontmatter.c parses a test file, runner.c runs one variant of it, main.c drives the suite.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define T262_NAME_MAX 96
#define T262_MAX_INCLUDES 24
#define T262_MAX_FEATURES 48
#define T262_REASON_MAX 192
/* Per-variant wall-clock limit. AddressSanitizer builds run several times slower than the
 * plain Debug build, so the limit is scaled there (the same check as EMB_TEST_ASAN in
 * tests/emb_test.h). */
#if defined(__SANITIZE_ADDRESS__)
#define T262_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define T262_ASAN 1
#endif
#endif
#ifndef T262_ASAN
#define T262_ASAN 0
#endif
#define T262_TIME_SCALE (T262_ASAN ? 4u : 1u)
/* 20 s at idle: the decodeURI and decodeURIComponent 4-byte sweeps take about 4.5 s per variant in
 * the Debug build and roughly double under load, so the 10 s limit they had made them flaky. */
#define T262_TIMEOUT_MS (20000u * T262_TIME_SCALE)

enum t262_flags {
    T262_FLAG_ONLY_STRICT = 1u << 0,
    T262_FLAG_NO_STRICT = 1u << 1,
    T262_FLAG_RAW = 1u << 2,
    T262_FLAG_ASYNC = 1u << 3,
    T262_FLAG_MODULE = 1u << 4,
    T262_FLAG_CAN_BLOCK = 1u << 5,
};

/* The frontmatter fields the runner reads. Lists that overflow their fixed capacity set
 * overflow, and the test is then skipped rather than run with a truncated list. */
typedef struct t262_meta {
    char includes[T262_MAX_INCLUDES][T262_NAME_MAX];
    int n_includes;
    char features[T262_MAX_FEATURES][T262_NAME_MAX];
    int n_features;
    unsigned flags;
    bool has_negative;
    char negative_phase[16];
    char negative_type[T262_NAME_MAX];
    bool overflow;
} t262_meta;

/* The result of one variant. NONE means the variant does not apply to the test. */
typedef enum t262_outcome {
    T262_NONE,
    T262_SKIP,
    T262_PASS,
    T262_FAIL,
    T262_TIMEOUT
} t262_outcome;

/* Variant indices used in per-test arrays. */
#define T262_SLOPPY 0
#define T262_STRICT 1

/* Parses the frontmatter of a test file. A file without frontmatter yields an empty meta. */
void t262_parse_meta(const char *src, size_t len, t262_meta *out);

/* Runs one variant of a test in a fresh runtime and realm. root is the test262 checkout (its
 * harness/ directory holds the harness files), rel is the test path used in messages. reason
 * receives a short explanation whenever the outcome is not PASS. */
t262_outcome t262_run(const char *root, const char *rel, const char *src, size_t len,
                      const t262_meta *meta, bool strict, char *reason, size_t reason_cap);

/* Collector settings for every runtime the runner creates (J5h). stress_every collects before
 * every N-th allocation (0 is off); generational turns on sticky marks and minor collections. Call
 * it once, before the workers start. */
void t262_set_gc(uint32_t stress_every, bool generational);

/* Rope threshold for every runtime the runner creates (J5g): concatenations of this many code
 * units or more build ropes. 0 keeps the engine default. The rope stress run uses 1. */
void t262_set_rope_min(uint32_t rope_min);

/* With --gc-stress every allocation (or every few) runs a full collection, which makes an
 * allocation-heavy variant (Function.prototype.toString over every built-in) take minutes in a
 * Debug build: the deadline is this many times longer then. */
#define T262_STRESS_SCALE 10u

/* The deadline of one variant: T262_TIMEOUT_MS, scaled by T262_STRESS_SCALE under --gc-stress. */
uint32_t t262_timeout_ms(void);
